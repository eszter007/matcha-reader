#include "DictIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

bool DictIndex::isAvailable() {
  return Storage.exists(IDX_PATH) && Storage.exists(DAT_PATH);
}

bool DictIndex::lookupExact(const char* headword, DictEntry& out) {
  HalFile idxFile;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idxFile)) {
    return false;
  }

  const size_t fileSize = idxFile.size();
  if (fileSize < sizeof(DictIndexRecord)) {
    idxFile.close();
    return false;
  }

  const size_t recordCount = fileSize / sizeof(DictIndexRecord);

  // Pad the search key to match the fixed-size headword field.
  char key[DictIndexRecord::HEADWORD_SIZE];
  std::memset(key, 0, sizeof(key));
  const size_t len = std::strlen(headword);
  if (len >= sizeof(key)) {
    idxFile.close();
    return false;
  }
  std::memcpy(key, headword, len);

  // Binary search over the sorted index.
  size_t lo = 0;
  size_t hi = recordCount;
  DictIndexRecord rec;

  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    idxFile.seek(mid * sizeof(DictIndexRecord));
    if (idxFile.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec)) {
      break;
    }

    const int cmp = std::memcmp(key, rec.headword, DictIndexRecord::HEADWORD_SIZE);
    if (cmp < 0) {
      hi = mid;
    } else if (cmp > 0) {
      lo = mid + 1;
    } else {
      idxFile.close();

      // Found — read the definition from the dat file.
      HalFile datFile;
      if (!Storage.openFileForRead("DICT", DAT_PATH, datFile)) {
        return false;
      }

      datFile.seek(rec.offset);
      std::string def;
      def.resize(rec.length);
      if (datFile.read(reinterpret_cast<uint8_t*>(def.data()), rec.length) != rec.length) {
        datFile.close();
        return false;
      }
      datFile.close();

      out.headword = headword;
      out.definition = std::move(def);
      out.priority = rec.priority;
      return true;
    }
  }

  idxFile.close();
  return false;
}
