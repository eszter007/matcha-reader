#include "GeminiConfig.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SdSystemDir.h>

#include <algorithm>

namespace gemini {
namespace {
constexpr const char* KEY_FILE = "gemini.key";
constexpr const char* MODEL_FILE = "gemini.model";

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Reads a small one-line file into out, without a UTF-8 BOM and surrounding whitespace. bufSize
// bounds the line: a full buffer means the file is not what it should be.
template <size_t N>
bool readTrimmed(const char* leaf, std::string& out) {
  char buf[N];
  size_t len = Storage.readFileToBuffer(sdsystem::findUserFile(leaf).c_str(), buf, sizeof(buf));
  if (len == 0) return false;
  if (len >= sizeof(buf) - 1) {
    LOG_ERR("GEM", "%s is %u+ bytes; ignoring it", leaf, static_cast<unsigned>(len));
    return false;
  }
  size_t start = 0;
  // A UTF-8 BOM is what an editor on Windows writes when the file is saved as UTF-8.
  if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB &&
      static_cast<unsigned char>(buf[2]) == 0xBF) {
    start = 3;
  }
  while (start < len && isSpace(buf[start])) start++;
  while (len > start && isSpace(buf[len - 1])) len--;
  if (len <= start) return false;
  out.assign(buf + start, len - start);
  return true;
}

bool writeLine(const char* leaf, const std::string& value) {
  const std::string path = sdsystem::findUserFile(leaf);
  if (value.empty()) return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("GEM", path, file)) return false;
  return file.write(reinterpret_cast<const uint8_t*>(value.data()), value.size()) == value.size();
}

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) a++;
  while (b > a && isSpace(s[b - 1])) b--;
  return s.substr(a, b - a);
}
}  // namespace

// 192 bytes: more than three times the longest key Google issues, and under the 256-byte stack rule.
bool readKey(std::string& keyOut) { return readTrimmed<192>(KEY_FILE, keyOut); }

bool writeKey(const std::string& key) { return writeLine(KEY_FILE, trimmed(key)); }

std::string maskedKey() {
  std::string key;
  if (!readKey(key)) return "";
  return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" + key.substr(key.size() > 4 ? key.size() - 4 : 0);
}

std::string readModel() {
  std::string model;
  if (!readTrimmed<64>(MODEL_FILE, model)) return "";
  // Model names are lower-case ASCII; anything else is not a name the API would accept.
  const bool foreign = std::any_of(model.begin(), model.end(), [](const char c) {
    return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_');
  });
  if (foreign) return "";
  return model == DEFAULT_MODEL ? "" : model;
}

bool writeModel(const std::string& model) {
  const std::string name = trimmed(model);
  return writeLine(MODEL_FILE, name == DEFAULT_MODEL ? "" : name);
}

ModelChoice modelChoice() {
  const std::string model = readModel();
  if (model.empty()) return ModelChoice::Default;
  return model == LITE_MODEL ? ModelChoice::Lite : ModelChoice::Custom;
}

}  // namespace gemini
