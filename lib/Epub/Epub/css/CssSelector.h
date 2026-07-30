#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

/**
 * CSS selector syntax: the subset this engine supports, the normalized key it is stored
 * under, and the candidate keys a given element can match.
 *
 * Header-only and free of Arduino/HAL/heap dependencies, so the exact code the firmware
 * runs is also what the host test (test/css_selector) exercises. Nothing here allocates.
 *
 * A "compound" is one of `tag`, `.class`, `tag.class`. Supported selector forms:
 *
 *   compound              simple     p / .note / p.note
 *   compound compound     descendant .callout p        (B anywhere inside A)
 *   compound > compound   child      blockquote > p    (B a direct child of A)
 *
 * THREE or more compounds (`.a .b p`) are REJECTED, not approximated by their rightmost
 * two: `.b p` matches everywhere, including outside `.a`, so the approximation applies
 * styling the author explicitly scoped away. A wrong match is worse than no match, and
 * dropping the rule is exactly the behaviour those selectors had before this existed.
 * Everything else -- attribute, pseudo, id, sibling, universal -- is rejected as before.
 */
namespace CssSelector {

// Stored-key form: the combinator sits between the two compounds with no padding, so
// `.callout   p` and `blockquote > p` normalize to `.callout p` and `blockquote>p`.
// Neither character can occur inside a compound, so a key parses back unambiguously and
// can never collide with a simple selector or with the "h|"/"v|" scope prefixes.
inline constexpr char DESCENDANT_COMBINATOR = ' ';
inline constexpr char CHILD_COMBINATOR = '>';

// Characters that introduce selector syntax this engine does not implement:
//   '+' adjacent sibling · '[' attribute · ':' pseudo · '#' id · '~' general sibling
//   '*' universal · '|' namespace (which is also the scope-prefix separator, so a
//       namespaced selector must never reach the map)
inline constexpr std::string_view UNSUPPORTED_CHARS = "+[]:#~*|";

// Specificity weights. The real cascade orders by the (ids, classes, types) triple; with
// no id support and at most two compounds, one class can never be outranked by any number
// of types, so a single byte with a base-16 class column reproduces the ordering exactly
// for every selector this engine stores (max value 2*16 + 2 = 34).
inline constexpr uint8_t CLASS_SPECIFICITY = 16;
inline constexpr uint8_t TYPE_SPECIFICITY = 1;

// Class names considered per element when matching a COMPOUND selector. Beyond this the
// extra classes are ignored (fewer matches, never a wrong one), which bounds the ancestor
// walk at (1 + 2*MAX_MATCH_CLASSES)^2 lookups per ancestor. Simple selectors are matched
// against every class in the attribute, uncapped, exactly as they were before compound
// selectors existed.
inline constexpr size_t MAX_MATCH_CLASSES = 3;

// tag + '.' + class, combinator, tag + '.' + class
inline constexpr size_t MAX_KEY_PIECES = 7;

inline constexpr bool isWhitespace(const char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/** One compound selector. `tag` empty means ".class"; `cls` empty means a bare tag. */
struct Compound {
  std::string_view tag;
  std::string_view cls;
  [[nodiscard]] constexpr uint8_t specificity() const {
    return static_cast<uint8_t>((tag.empty() ? 0 : TYPE_SPECIFICITY) + (cls.empty() ? 0 : CLASS_SPECIFICITY));
  }
};

/** A parsed selector: `subject`, optionally scoped by `ancestor` through `combinator`. */
struct Selector {
  Compound ancestor;    // meaningful only when combinator != 0
  Compound subject;     // the element the rule styles (the rightmost compound)
  char combinator = 0;  // 0 = simple, DESCENDANT_COMBINATOR or CHILD_COMBINATOR
  [[nodiscard]] constexpr uint8_t specificity() const {
    return static_cast<uint8_t>(subject.specificity() + (combinator ? ancestor.specificity() : 0));
  }
};

/** Parse one compound. Returns false for anything outside `tag` / `.class` / `tag.class`. */
inline bool parseCompound(const std::string_view s, Compound& out) {
  if (s.empty()) return false;
  size_t dot = std::string_view::npos;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.') {
      if (dot != std::string_view::npos) return false;  // .a.b -- multi-class compounds are not matched
      dot = i;
      continue;
    }
    if (isWhitespace(c) || c == CHILD_COMBINATOR) return false;
    if (UNSUPPORTED_CHARS.find(c) != std::string_view::npos) return false;
  }
  if (dot == std::string_view::npos) {
    out = Compound{s, {}};
    return true;
  }
  if (dot + 1 >= s.size()) return false;  // trailing '.'
  out = Compound{s.substr(0, dot), s.substr(dot + 1)};
  return true;
}

/**
 * Parse a single selector (no commas -- the caller splits selector lists first).
 * Returns false if the selector uses syntax this engine does not implement, including
 * three or more compounds. Views in `out` point into `sel`.
 */
inline bool parse(const std::string_view sel, Selector& out) {
  Compound compounds[2];
  size_t count = 0;
  char combinator = 0;
  bool pendingChild = false;

  size_t i = 0;
  while (i < sel.size()) {
    const char c = sel[i];
    if (isWhitespace(c)) {
      ++i;
      continue;
    }
    if (c == CHILD_COMBINATOR) {
      if (count == 0) return false;  // leading '>'
      pendingChild = true;
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < sel.size() && !isWhitespace(sel[i]) && sel[i] != CHILD_COMBINATOR) ++i;
    if (count == 2) return false;  // three or more compounds -- see the header comment
    Compound cp;
    if (!parseCompound(sel.substr(start, i - start), cp)) return false;
    if (count == 1) combinator = pendingChild ? CHILD_COMBINATOR : DESCENDANT_COMBINATOR;
    compounds[count++] = cp;
    pendingChild = false;
  }

  if (count == 0 || pendingChild) return false;  // empty, or a trailing combinator
  if (count == 1) {
    out = Selector{{}, compounds[0], 0};
    return true;
  }
  out = Selector{compounds[0], compounds[1], combinator};
  return true;
}

/**
 * Feed the parsed selector's storage key to `sink` piece by piece; concatenated they spell
 * the normalized key (`.callout p`, `blockquote>p`). Building it this way keeps the choice of
 * buffer with the caller -- the parser appends into a std::string whose small-string buffer
 * holds every realistic selector, so a stored rule costs no extra allocation.
 */
template <typename Sink>
void forEachKeyPiece(const Selector& sel, Sink&& sink) {
  static constexpr std::string_view DOT = ".";
  static constexpr std::string_view DESCENDANT_PIECE = " ";
  static constexpr std::string_view CHILD_PIECE = ">";
  const auto emitCompound = [&sink](const Compound& c) {
    if (!c.tag.empty()) sink(c.tag);
    if (!c.cls.empty()) {
      sink(DOT);
      sink(c.cls);
    }
  };
  if (sel.combinator != 0) {
    emitCompound(sel.ancestor);
    sink(sel.combinator == CHILD_COMBINATOR ? CHILD_PIECE : DESCENDANT_PIECE);
  }
  emitCompound(sel.subject);
}

/** An element as the matcher sees it: its tag and its raw space-separated class attribute. */
struct ElementRef {
  std::string_view tag;
  std::string_view classAttr;
};

/** Invoke fn(name) for every whitespace-separated class name in a class attribute. */
template <typename F>
void forEachClassName(const std::string_view attr, F&& fn) {
  size_t i = 0;
  while (i < attr.size()) {
    while (i < attr.size() && isWhitespace(attr[i])) ++i;
    const size_t start = i;
    while (i < attr.size() && !isWhitespace(attr[i])) ++i;
    if (i > start) fn(attr.substr(start, i - start));
  }
}

/** The first MAX_MATCH_CLASSES class names of a class attribute, as views into it. */
struct ClassList {
  std::string_view items[MAX_MATCH_CLASSES];
  size_t count = 0;
};

inline ClassList splitClasses(const std::string_view attr) {
  ClassList out;
  forEachClassName(attr, [&out](const std::string_view name) {
    if (out.count < MAX_MATCH_CLASSES) out.items[out.count++] = name;
  });
  return out;
}

/**
 * Number of compound forms an element can match: the bare tag, `.class` per class, and
 * `tag.class` per class.
 */
inline size_t formCount(const std::string_view tag, const ClassList& classes) {
  const size_t withTag = tag.empty() ? 0 : 1;
  return withTag + classes.count * (withTag ? 2 : 1);
}

/**
 * Write form `f` of an element as key pieces (concatenated they spell the selector) and
 * report its specificity. Returns the number of pieces written (<= 3).
 * Form order: 0 = tag, then `.class_i`, then `tag.class_i` -- ascending specificity.
 */
inline size_t fillForm(const std::string_view tag, const ClassList& classes, size_t f, std::string_view* pieces,
                       uint8_t& spec) {
  const bool hasTag = !tag.empty();
  if (hasTag && f == 0) {
    pieces[0] = tag;
    spec = TYPE_SPECIFICITY;
    return 1;
  }
  size_t idx = f - (hasTag ? 1 : 0);
  if (idx < classes.count) {
    pieces[0] = ".";
    pieces[1] = classes.items[idx];
    spec = CLASS_SPECIFICITY;
    return 2;
  }
  idx -= classes.count;
  pieces[0] = tag;
  pieces[1] = ".";
  pieces[2] = classes.items[idx];
  spec = static_cast<uint8_t>(TYPE_SPECIFICITY + CLASS_SPECIFICITY);
  return 3;
}

/**
 * Enumerate every stored key `subject` could match, calling
 *   emit(pieces, pieceCount, specificity, isSimple)
 * once per candidate. Concatenating `pieces` gives the key; the caller looks it up without
 * materializing the concatenation.
 *
 * `ancestorAt(i)` returns the i-th recorded ancestor, OUTERMOST first; the caller
 * guarantees every one of them really is an ancestor (a truncated stack drops inner
 * levels, which can only lose matches, never invent one). `parentIsLast` says whether
 * ancestorAt(ancestorCount-1) is the element's immediate parent -- the child combinator
 * is tried only then.
 *
 * Emission order defines the tie-break among equal-specificity rules (the caller applies
 * later-emitted over earlier): simple before compound, outer ancestor before inner,
 * descendant before child.
 */
template <typename AncestorAt, typename Emit>
void forEachCandidate(const ElementRef& subject, const AncestorAt& ancestorAt, const size_t ancestorCount,
                      const bool parentIsLast, const bool tryDescendant, const bool tryChild, Emit&& emit) {
  std::string_view pieces[MAX_KEY_PIECES];

  // Simple selectors, over EVERY class in the attribute (no cap: this is the pre-existing
  // lookup set and must not lose a match). The caller sorts by specificity, so `.a` and
  // `p.a` may be emitted together per class.
  if (!subject.tag.empty()) {
    pieces[0] = subject.tag;
    emit(static_cast<const std::string_view*>(pieces), 1, TYPE_SPECIFICITY, true);
  }
  forEachClassName(subject.classAttr, [&](const std::string_view cls) {
    pieces[0] = ".";
    pieces[1] = cls;
    emit(static_cast<const std::string_view*>(pieces), 2, CLASS_SPECIFICITY, true);
    if (subject.tag.empty()) return;
    pieces[0] = subject.tag;
    pieces[1] = ".";
    pieces[2] = cls;
    emit(static_cast<const std::string_view*>(pieces), 3, static_cast<uint8_t>(TYPE_SPECIFICITY + CLASS_SPECIFICITY),
         true);
  });

  if (!tryDescendant && !tryChild) return;

  const ClassList subjectClasses = splitClasses(subject.classAttr);
  const size_t subjectForms = formCount(subject.tag, subjectClasses);

  static constexpr std::string_view DESCENDANT_PIECE = " ";
  static constexpr std::string_view CHILD_PIECE = ">";

  for (size_t a = 0; a < ancestorCount; ++a) {
    const ElementRef anc = ancestorAt(a);
    if (anc.tag.empty() && anc.classAttr.empty()) continue;
    const ClassList ancClasses = splitClasses(anc.classAttr);
    const size_t ancForms = formCount(anc.tag, ancClasses);
    const bool isParent = parentIsLast && a + 1 == ancestorCount;

    for (int pass = 0; pass < 2; ++pass) {
      const bool child = pass == 1;
      if (child ? !(tryChild && isParent) : !tryDescendant) continue;

      for (size_t af = 0; af < ancForms; ++af) {
        uint8_t ancSpec = 0;
        const size_t an = fillForm(anc.tag, ancClasses, af, pieces, ancSpec);
        pieces[an] = child ? CHILD_PIECE : DESCENDANT_PIECE;
        for (size_t sf = 0; sf < subjectForms; ++sf) {
          uint8_t subSpec = 0;
          const size_t sn = fillForm(subject.tag, subjectClasses, sf, pieces + an + 1, subSpec);
          emit(static_cast<const std::string_view*>(pieces), an + 1 + sn, static_cast<uint8_t>(ancSpec + subSpec),
               false);
        }
      }
    }
  }
}

}  // namespace CssSelector

/**
 * The chain of currently open elements, maintained by the HTML walk so descendant and
 * child selectors can be matched.
 *
 * Fixed capacity, zero heap: one inline entry per open element, pushed on element open and
 * popped on close. Depth beyond MAX_DEPTH is COUNTED but not recorded, which keeps the
 * recorded entries a prefix of the real ancestor chain -- every recorded entry is still a
 * genuine ancestor, so an over-deep document loses matches instead of inventing them.
 * A tag or class name that does not fit its buffer is dropped for the same reason
 * (an unknown name matches nothing rather than something wrong).
 *
 * Cost: sizeof(Entry) * MAX_DEPTH bytes, allocated once with the parser.
 */
class CssElementPath {
 public:
  static constexpr size_t MAX_DEPTH = 12;
  static constexpr size_t MAX_TAG_LEN = 15;
  static constexpr size_t MAX_CLASS_LEN = 31;

  void clear() { depth_ = 0; }

  /** Open an element. Always call exactly once per start tag, before resolving its style. */
  void push(const char* tag) {
    if (depth_ < MAX_DEPTH) {
      Entry& e = entries_[depth_];
      e.tag[0] = '\0';
      e.classes[0] = '\0';
      if (tag != nullptr) {
        const size_t len = strlen(tag);
        if (len > 0 && len <= MAX_TAG_LEN) {
          memcpy(e.tag, tag, len);
          e.tag[len] = '\0';
        }
      }
    }
    ++depth_;
  }

  /** Close an element. Always call exactly once per end tag. */
  void pop() {
    if (depth_ > 0) --depth_;
  }

  /**
   * Record the class attribute of the element just pushed. Class names are copied whole
   * while they fit; an overflowing name is dropped rather than truncated, since a
   * truncated name could collide with a different, real class.
   */
  void setTopClasses(const std::string_view classAttr) {
    if (depth_ == 0 || depth_ > MAX_DEPTH) return;
    char* dst = entries_[depth_ - 1].classes;
    size_t used = 0;
    dst[0] = '\0';
    size_t i = 0;
    while (i < classAttr.size()) {
      while (i < classAttr.size() && CssSelector::isWhitespace(classAttr[i])) ++i;
      const size_t start = i;
      while (i < classAttr.size() && !CssSelector::isWhitespace(classAttr[i])) ++i;
      const size_t len = i - start;
      if (len == 0) continue;
      const size_t need = used == 0 ? len : len + 1;
      if (used + need > MAX_CLASS_LEN) continue;
      if (used != 0) dst[used++] = ' ';
      memcpy(dst + used, classAttr.data() + start, len);
      used += len;
      dst[used] = '\0';
    }
  }

  /** Number of recorded ancestors of the element currently on top, outermost first. */
  [[nodiscard]] size_t ancestorCount() const {
    if (depth_ == 0) return 0;
    return depth_ - 1 < MAX_DEPTH ? depth_ - 1 : MAX_DEPTH;
  }

  [[nodiscard]] CssSelector::ElementRef ancestor(const size_t i) const {
    const Entry& e = entries_[i];
    return CssSelector::ElementRef{e.tag, e.classes};
  }

  /** True when ancestor(ancestorCount()-1) is the immediate parent (child combinator is valid). */
  [[nodiscard]] bool parentRecorded() const { return depth_ >= 2 && depth_ - 2 < MAX_DEPTH; }

 private:
  struct Entry {
    char tag[MAX_TAG_LEN + 1];
    char classes[MAX_CLASS_LEN + 1];
  };
  Entry entries_[MAX_DEPTH]{};
  size_t depth_ = 0;
};
