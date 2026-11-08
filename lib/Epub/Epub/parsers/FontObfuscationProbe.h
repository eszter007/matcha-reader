#pragma once

#include <Print.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "../FontObfuscation.h"

// Base for the two small streaming XML readers below: feed it with ZipFile::readFileToStream.
class XmlProbe : public Print {
 public:
  ~XmlProbe() override { destroyXmlParser(parser); }
  XmlProbe() = default;
  XmlProbe(const XmlProbe&) = delete;
  XmlProbe& operator=(const XmlProbe&) = delete;

  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!parser || size > INT_MAX ||
        XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(size), XML_FALSE) == XML_STATUS_ERROR) {
      return 0;
    }
    return size;
  }
  bool finish() { return parser && XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_OK; }

 protected:
  bool setupParser(XML_StartElementHandler start, XML_EndElementHandler end, XML_CharacterDataHandler text) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return false;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, start, end);
    if (text) XML_SetCharacterDataHandler(parser, text);
    return true;
  }
  static const char* attr(const XML_Char** atts, const char* localName) {
    for (size_t i = 0; atts[i]; i += 2) {
      if (xmlLocalNameEquals(atts[i], localName)) return atts[i + 1];
    }
    return nullptr;
  }

 private:
  XML_Parser parser = nullptr;
};

// META-INF/encryption.xml: which resources are obfuscated, and how. Other ciphers are ignored.
class FontObfuscationProbe : public XmlProbe {
 public:
  struct Entry {
    std::string path;  // zip path, relative to the container root
    fontobfuscation::Scheme scheme;
  };
  static constexpr size_t MAX_ENTRIES = 16;

  bool setup() {
    entries.reserve(4);
    return setupParser(onStart, nullptr, nullptr);
  }
  const std::vector<Entry>& obfuscated() const { return entries; }

 private:
  std::vector<Entry> entries;
  fontobfuscation::Scheme pending = fontobfuscation::Scheme::None;

  static void XMLCALL onStart(void* ctx, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<FontObfuscationProbe*>(ctx);
    if (xmlLocalNameEquals(name, "EncryptedData")) {
      self->pending = fontobfuscation::Scheme::None;
    } else if (xmlLocalNameEquals(name, "EncryptionMethod")) {
      const char* algorithm = attr(atts, "Algorithm");
      self->pending = algorithm ? fontobfuscation::schemeForAlgorithm(algorithm) : fontobfuscation::Scheme::None;
    } else if (xmlLocalNameEquals(name, "CipherReference")) {
      const char* uri = attr(atts, "URI");
      if (uri && *uri && self->pending != fontobfuscation::Scheme::None && self->entries.size() < MAX_ENTRIES) {
        self->entries.push_back({uri, self->pending});
      }
    }
  }
};

// content.opf: the package's unique identifier, the key every obfuscated font is XORed with.
class UniqueIdentifierProbe : public XmlProbe {
 public:
  bool setup() { return setupParser(onStart, onEnd, onText); }
  const std::string& identifier() const { return value; }

 private:
  static constexpr size_t MAX_ID = 256;
  std::string wantedId;
  std::string value;
  bool capturing = false;
  bool done = false;

  static void XMLCALL onStart(void* ctx, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<UniqueIdentifierProbe*>(ctx);
    if (self->done) return;
    if (xmlLocalNameEquals(name, "package")) {
      const char* id = attr(atts, "unique-identifier");
      if (id) self->wantedId = id;
    } else if (xmlLocalNameEquals(name, "identifier") && !self->wantedId.empty()) {
      const char* id = attr(atts, "id");
      if (id && self->wantedId == id) {
        self->capturing = true;
        self->value.clear();
      }
    }
  }
  static void XMLCALL onEnd(void* ctx, const XML_Char* name) {
    auto* self = static_cast<UniqueIdentifierProbe*>(ctx);
    if (self->capturing && xmlLocalNameEquals(name, "identifier")) {
      self->capturing = false;
      self->done = true;
    }
  }
  static void XMLCALL onText(void* ctx, const XML_Char* s, const int len) {
    auto* self = static_cast<UniqueIdentifierProbe*>(ctx);
    if (!self->capturing || len <= 0) return;
    const size_t room = MAX_ID - std::min(self->value.size(), MAX_ID);
    self->value.append(s, std::min(static_cast<size_t>(len), room));
  }
};
