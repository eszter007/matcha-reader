#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <MatchaContainer.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>
#endif

#include <algorithm>
#include <cassert>

#define SDCard SDCardManager::getInstance()

namespace {
#if FREEINK_CAP_USB_MSC
freeink::UsbMassStorage usbMassStorage;
#endif
}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

void HalStorage::prepareForDeepSleep() {
  StorageLock lock;
  SDCard.shutdown();
}

bool HalStorage::beginUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SD filesystem");
    return false;
  }

  if (!usbMassStorage.begin(blockDevice)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.disconnectHost();
#else
  return false;
#endif
}

bool HalStorage::usbDriveHostSuspended() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  return usbMassStorage.hostSuspended();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  usbMassStorage.end();
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  StorageLock lock;
  switch (usbMassStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

// Composed of already-locked HalStorage/HalFile operations; needs no
// StorageLock of its own.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  // string growth is a bare allocation under -fno-exceptions; probe first so
  // a large file on a fragmented heap fails soft instead of abort()ing.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size + 512) {
    LOG_ERR(moduleName, "readFileToString OOM: %u bytes for %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
  // Set for an entry of a .matcha container: the file is the container, read through a window.
  bool windowed = false;
  uint32_t windowStart = 0;
  uint32_t windowLength = 0;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  if (std::string container, entry; matcha::splitPath(path, container, entry)) {
    LOG_ERR("HAL", "open() cannot reach .matcha entries, use openFileForRead: %s", path);
  }
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

namespace {
bool readContainerAt(void* ctx, const uint32_t offset, void* buf, const size_t n) {
  auto& file = *static_cast<HalFile*>(ctx);
  return file.seekSet(offset) && file.read(buf, n) == static_cast<int>(n);
}
}  // namespace

bool HalStorage::exists(const char* path) {
  std::string container, entry;
  if (matcha::splitPath(path, container, entry)) {
    HalFile file;
    matcha::Entry found;
    return openFileForRead("HAL", container, file) && matcha::findEntry(readContainerAt, &file, entry, found);
  }
  HAL_STORAGE_WRAPPED_CALL(exists, path);
}

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  HAL_STORAGE_WRAPPED_CALL(replaceFile, tmpPath, path);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  std::string container, entry;
  if (matcha::splitPath(path, container, entry)) {
    matcha::Entry found;
    if (!openFileForRead(moduleName, container, file) || !matcha::findEntry(readContainerAt, &file, entry, found) ||
        found.isDirectory() || static_cast<uint64_t>(found.offset) + found.size > file.fileSize64() ||
        !file.setWindow(found.offset, found.size)) {
      file = HalFile();
      return false;
    }
    return true;
  }
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForAppend(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile = SDCard.open(path, O_WRONLY | O_CREAT | O_APPEND);
  const bool ok = static_cast<bool>(fsFile);
  if (!ok) LOG_ERR(moduleName, "Failed to open file for append: %s", path);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
bool HalFile::setWindow(const uint32_t offset, const uint32_t length) {
  HalStorage::StorageLock lock;
  impl->windowed = true;
  impl->windowStart = offset;
  impl->windowLength = length;
  return impl->file.seekSet(offset);
}

size_t HalFile::size() {
  if (impl && impl->windowed) return impl->windowLength;
  HAL_FILE_FORWARD_CALL(size, );  // already thread-safe, no need to wrap
}
size_t HalFile::fileSize() {
  if (impl && impl->windowed) return impl->windowLength;
  HAL_FILE_FORWARD_CALL(fileSize, );  // already thread-safe, no need to wrap
}

uint32_t HalFile::modifiedStamp() {
  if (!impl) return 0;
  HalStorage::StorageLock lock;
  uint16_t date = 0, time = 0;
  if (!impl->file.getModifyDateTime(&date, &time)) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
uint64_t HalFile::fileSize64() {
  if (impl && impl->windowed) return impl->windowLength;
  HAL_FILE_FORWARD_CALL(fileSize, );  // already thread-safe, no need to wrap
}
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl || !impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) { return seekSet(pos); }
bool HalFile::seek64(uint64_t pos) {
  if (impl && impl->windowed) return pos <= impl->windowLength && seekSet(static_cast<size_t>(pos));
  HAL_FILE_WRAPPED_CALL(seekSet, pos);
}
bool HalFile::seekCur(int64_t offset) {
  if (impl && impl->windowed) {
    const int64_t target = static_cast<int64_t>(position()) + offset;
    return target >= 0 && seekSet(static_cast<size_t>(target));
  }
  HAL_FILE_WRAPPED_CALL(seekCur, offset);
}
bool HalFile::seekSet(size_t offset) {
  if (impl && impl->windowed) {
    if (offset > impl->windowLength) return false;
    HalStorage::StorageLock lock;
    return impl->file.seekSet(impl->windowStart + offset);
  }
  HAL_FILE_WRAPPED_CALL(seekSet, offset);
}
bool HalFile::truncate(const uint64_t length) { HAL_FILE_WRAPPED_CALL(truncate, length); }
int HalFile::available() const {
  if (impl && impl->windowed) return static_cast<int>(impl->windowLength - position());
  HAL_FILE_WRAPPED_CALL(available, );
}
size_t HalFile::position() const {
  if (impl && impl->windowed) {
    HalStorage::StorageLock lock;
    const uint64_t at = impl->file.position();
    return at > impl->windowStart ? static_cast<size_t>(at - impl->windowStart) : 0;
  }
  HAL_FILE_WRAPPED_CALL(position, );
}
int HalFile::read(void* buf, size_t count) {
  if (impl && impl->windowed) {
    const size_t left = impl->windowLength - std::min<size_t>(position(), impl->windowLength);
    if (count > left) count = left;
    if (count == 0) return 0;
  }
  HAL_FILE_WRAPPED_CALL(read, buf, count);
}
int HalFile::read() {
  if (impl && impl->windowed && position() >= impl->windowLength) return -1;
  HAL_FILE_WRAPPED_CALL(read, );
}
size_t HalFile::write(const uint8_t* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
