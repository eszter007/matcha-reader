#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  // ArduinoJson does not fail when its pool runs out of heap: it sets overflowed() and silently
  // drops every key that did not fit. Writing that document replaces a complete file with a
  // truncated one, and each missing key then loads back as its default -- a setting that "reverts"
  // with nothing in the log. Keep the previous file instead; a later save can retry.
  if (doc.overflowed()) {
    LOG_ERR("PERSIST", "Document for %s overflowed on low heap; keeping the previous file", path);
    return false;
  }
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  // A String that cannot grow stops short without an error, and the short document would replace
  // a complete file. Write only what serialized in full.
  if (json.length() != measureJson(doc)) {
    LOG_ERR("PERSIST", "Serializing %s fell short on low heap; keeping the previous file", path);
    return false;
  }
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  String json;
  if (Storage.exists(path)) {
    json = Storage.readFile(path);
  } else {
    // writeFile() swaps in a complete <path>.tmp by removing <path> and renaming; a stop between
    // the two leaves only the copy.
    const String tmp = String(path) + ".tmp";
    if (!Storage.exists(tmp.c_str())) return false;  // Expected on first boot -- not an error.
    LOG_INF("PERSIST", "Recovering %s from %s", path, tmp.c_str());
    json = Storage.readFile(tmp.c_str());
  }
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
