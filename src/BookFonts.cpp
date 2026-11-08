#include "BookFonts.h"

#include <Epub.h>
#include <Epub/BookFontMap.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

BookFonts bookFonts;

#if CROSSPOINT_VECTOR_FONTS

#include <Epub/FontObfuscation.h>
#include <Epub/css/CssParser.h>
#include <Epub/parsers/FontObfuscationProbe.h>
#include <FontPsram.h>
#include <FsHelpers.h>
#include <Print.h>
#include <TtfEpdFont.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>

#include <string>
#include <vector>

namespace {
// A publisher font bigger than this is not a text face (a CJK font is several MB): left out.
constexpr size_t MAX_FONT_BYTES = 4 * 1024 * 1024;
// PSRAM that must stay free after a font file is placed there, for glyph caches and decoders.
constexpr size_t PSRAM_HEADROOM = 512 * 1024;

uint32_t fnv(uint32_t h, const uint32_t v) { return (h ^ v) * 16777619u; }

uint32_t fnvString(uint32_t h, const std::string& s) {
  for (const char c : s) h = fnv(h, static_cast<uint8_t>(c));
  return h;
}

// Writes a zip entry straight into a PSRAM buffer reserved to its inflated size.
class PsramSink : public Print {
 public:
  explicit PsramSink(freeink::font::PsramVector<uint8_t>& out) : out_(out) {}
  size_t write(const uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, const size_t size) override {
    if (out_.size() + size > out_.capacity()) return 0;  // never regrow: the size was checked
    out_.insert(out_.end(), data, data + size);
    return size;
  }

 private:
  freeink::font::PsramVector<uint8_t>& out_;
};

struct Obfuscation {
  std::vector<FontObfuscationProbe::Entry> entries;
  std::string identifier;

  fontobfuscation::Scheme schemeFor(const std::string& path) const {
    for (const auto& e : entries) {
      if (FsHelpers::normalisePath(e.path) == path) return e.scheme;
    }
    return fontobfuscation::Scheme::None;
  }
};

Obfuscation readObfuscation(const Epub& epub) {
  Obfuscation o;
  size_t manifestSize = 0;
  if (!ZipFile(epub.getPath()).getInflatedFileSize("META-INF/encryption.xml", &manifestSize)) return o;
  FontObfuscationProbe probe;
  if (!probe.setup() || !ZipFile(epub.getPath()).readFileToStream("META-INF/encryption.xml", probe, 512) ||
      !probe.finish()) {
    LOG_ERR("BFNT", "Cannot read encryption.xml");
    return o;
  }
  o.entries = probe.obfuscated();
  if (o.entries.empty()) return o;
  o.identifier = epub.readUniqueIdentifier();
  if (o.identifier.empty()) LOG_ERR("BFNT", "Obfuscated fonts but no unique identifier");
  return o;
}

int familyFontId(const uint16_t familyKey, const uint8_t pointSize) {
  uint32_t h = fnv(fnv(fnv(2166136261u, 0x424B464Eu /* "BKFN" */), familyKey), pointSize);
  const int id = static_cast<int>(h);
  return id != 0 ? id : 1;
}
}  // namespace

struct BookFonts::Family {
  uint16_t familyKey = 0;
  int fontId = 0;
  freeink::font::PsramVector<uint8_t> bytes[4];
  TtfEpdFont font;
};

BookFonts::BookFonts() = default;
BookFonts::~BookFonts() = default;

uint32_t BookFonts::load(const Epub& epub, GfxRenderer& renderer, const uint8_t pointSize, const int readerFontId) {
  const uint32_t want =
      fnv(fnv(fnvString(2166136261u, epub.getPath()), pointSize), static_cast<uint32_t>(readerFontId));
  if (want == loadedFor_) {
    // The SD font system clears every fallback when it reloads; put ours back.
    for (const auto& family : families_) {
      if (family && readerFontId != 0) renderer.setFallbackFont(family->fontId, readerFontId);
    }
    return signature_;
  }
  unload(renderer);
  // Attempted once per book, size and fallback: a font that cannot load now is retried when the
  // book is opened again, not on every page.
  loadedFor_ = want;

  const std::vector<CssParser::FontFaceRule> faces = CssParser::loadFontFaces(epub.getCachePath());
  if (faces.empty()) return 0;
  const Obfuscation obfuscation = readObfuscation(epub);

  uint32_t signature = 2166136261u;
  int loaded = 0;
  for (size_t first = 0; first < faces.size() && loaded < MAX_FAMILIES; ++first) {
    const uint16_t key = faces[first].familyKey;
    bool seen = false;
    for (size_t i = 0; i < first; ++i) seen = seen || faces[i].familyKey == key;
    if (seen) continue;

    auto family = makeUniqueNoThrow<Family>();
    if (!family) {
      LOG_ERR("BFNT", "OOM: font family");
      break;
    }
    family->familyKey = key;
    bool anySource = false;
    for (const auto& face : faces) {
      if (face.familyKey != key) continue;
      const uint8_t style = (face.bold ? TtfEpdFont::Bold : 0) | (face.italic ? TtfEpdFont::Italic : 0);
      auto& bytes = family->bytes[style];
      if (!bytes.empty()) continue;
      const std::string path = FsHelpers::normalisePath(face.href);
      size_t size = 0;
      if (!epub.getItemSize(path, &size) || size == 0 || size > MAX_FONT_BYTES) {
        LOG_ERR("BFNT", "Font %s missing or too large (%u bytes)", path.c_str(), static_cast<unsigned>(size));
        continue;
      }
      // PSRAM only: up to eight faces stay for the whole book, and no cache release frees them.
      // PsramAlloc aborts on failure, so the reservation is checked first.
      if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size + PSRAM_HEADROOM) {
        LOG_ERR("BFNT", "No PSRAM for %s (%u bytes)", path.c_str(), static_cast<unsigned>(size));
        continue;
      }
      bytes.reserve(size);
      PsramSink sink(bytes);
      if (!epub.readItemContentsToStream(path, sink, 1024) || bytes.size() != size) {
        LOG_ERR("BFNT", "Short read on %s", path.c_str());
        freeink::font::PsramVector<uint8_t>().swap(bytes);
        continue;
      }
      const fontobfuscation::Scheme scheme = obfuscation.schemeFor(path);
      if (scheme != fontobfuscation::Scheme::None) {
        uint8_t key20[20];
        const size_t keyLen = fontobfuscation::deriveKey(scheme, obfuscation.identifier, key20);
        if (keyLen == 0) {
          freeink::font::PsramVector<uint8_t>().swap(bytes);
          continue;
        }
        fontobfuscation::deobfuscate(scheme, key20, keyLen, bytes.data(), bytes.size());
      }
      anySource = true;
    }
    if (!anySource) continue;

    // A family without a regular face takes its first face as the regular one.
    if (family->bytes[TtfEpdFont::Regular].empty()) {
      for (auto& bytes : family->bytes) {
        if (!bytes.empty()) {
          family->bytes[TtfEpdFont::Regular].swap(bytes);
          break;
        }
      }
    }
    for (uint8_t style = 0; style < 4; ++style) {
      const auto& bytes = family->bytes[style];
      if (!bytes.empty()) family->font.addResidentSource(style, bytes.data(), static_cast<uint32_t>(bytes.size()));
    }
    if (!family->font.load(pointSize)) {
      LOG_ERR("BFNT", "FreeType could not open family %04x", key);
      continue;
    }
    family->fontId = familyFontId(key, pointSize);
    renderer.insertFont(family->fontId, family->font.family());
    renderer.registerTtfFont(family->fontId, &family->font);
    if (readerFontId != 0) renderer.setFallbackFont(family->fontId, readerFontId);
    uint32_t styles = 0;  // a style left out for want of PSRAM lays out differently
    for (uint8_t style = 0; style < 4; ++style) styles |= (family->bytes[style].empty() ? 0u : 1u) << style;
    signature = fnv(fnv(fnv(signature, key), static_cast<uint32_t>(family->fontId)), styles);
    LOG_INF("BFNT", "Publisher font %04x loaded at %upt", key, pointSize);
    families_[loaded++] = std::move(family);
  }

  if (loaded == 0) return 0;
  uint16_t keys[MAX_FAMILIES] = {};
  int ids[MAX_FAMILIES] = {};
  for (int i = 0; i < loaded; ++i) {
    keys[i] = families_[i]->familyKey;
    ids[i] = families_[i]->fontId;
  }
  bookfonts::setFamilies(keys, ids, static_cast<size_t>(loaded));
  signature_ = signature != 0 ? signature : 1;
  return signature_;
}

void BookFonts::unload(GfxRenderer& renderer) {
  bookfonts::clear();
  for (auto& family : families_) {
    if (!family) continue;
    renderer.unregisterTtfFont(family->fontId);
    renderer.removeFont(family->fontId);
    family.reset();
  }
  signature_ = 0;
  loadedFor_ = 0;
}

#else

BookFonts::BookFonts() = default;
BookFonts::~BookFonts() = default;
uint32_t BookFonts::load(const Epub&, GfxRenderer&, uint8_t, int) { return 0; }
void BookFonts::unload(GfxRenderer&) {}

#endif
