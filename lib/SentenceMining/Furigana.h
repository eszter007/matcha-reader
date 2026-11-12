#pragma once

#include <string>
#include <string_view>

// Furigana in Anki's syntax: base[reading], a base running back to the previous space or ']'
// ("木[こ]漏[も]れ 日[び]"). Pure string work, host-testable.
namespace furigana {

// Appends base[reading] to `out`, with the space Anki needs when plain text precedes it.
void appendRuby(std::string& out, std::string_view base, std::string_view reading);

// The kana a furigana string reads as: its readings and plain text, katakana folded to hiragana.
std::string kanaOf(std::string_view text);

// True when `text` reads as one of `readings` ("きょう、こんにち": alternatives separated by 、).
bool matchesReading(std::string_view text, std::string_view readings);

// Carries furigana from the form on the page to the dictionary form: 止[や]めて and 止める give
// 止[や]める. Bases and plain text are kept while the headword repeats them; what is left of the
// headword must be kana. Empty when the two do not share the annotated kanji.
std::string adaptToHeadword(std::string_view surface, std::string_view headword);

}  // namespace furigana
