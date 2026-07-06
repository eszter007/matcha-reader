#pragma once

#include <string>

class Epub;

// Extracts the plain text of a footnote so the reader can show it in a panel without
// navigating away from the page (mirrors how Word Lookup shows definitions in place).
//
// `href` is a footnote reference target as collected by the section parser, e.g.
// "defNotesFin16.html#N19" or "#note_3" (same-file). The target spine item is streamed to a
// temp file in the book's cache dir and scanned with expat for the element carrying the
// anchor id; that element's subtree text is collected (whitespace-collapsed, capped).
// Anchors that sit on an empty inline marker (<a id="x"/>) extend collection to the parent
// block so the note body isn't missed.
namespace FootnoteText {

// Returns false when the href can't be resolved or the target can't be read; `out` is then a
// best-effort empty string. maxBytes caps the collected UTF-8 text (never splits mid-glyph).
bool extract(Epub& epub, int currentSpineIndex, const std::string& href, std::string& out,
             size_t maxBytes = 2048);

}  // namespace FootnoteText
