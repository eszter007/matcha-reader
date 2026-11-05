#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Reading of a number written in digits followed by a counter: 6 + 月 -> ろくがつ, 100 + 本 ->
// ひゃっぽん, 20 + 日 -> はつか, 3 + 日目 -> みっかめ, 5 + 万円 -> ごまんえん, 5 + km ->
// ごキロメートル. Counters outside the table return false and the dictionary's reading stands.
namespace CounterReading {

// Parses ASCII and full-width digits. False when `digits` is empty, holds another character or
// exceeds 10^15.
bool parseNumber(std::string_view digits, uint64_t& out);

// Byte length of the longest known counter at the start of `text` (a 万/億/兆 multiplier and a
// trailing 目 included), or 0.
size_t matchCounter(std::string_view text);

// Byte length of a leading 万/億/兆/千 multiplier, or 0.
size_t multiplierLength(std::string_view text);

// The reading of the counter after a plain number (本 -> ほん), or nullptr, also for 日 whose
// dictionary entry of that reading is another word. `counter` as matched by matchCounter.
const char* plainReading(std::string_view counter);

// The dictionary headword for a Latin-script unit (km -> キロメートル), or nullptr.
const char* unitWord(std::string_view counter);

// Hiragana reading of `number` + `counter` (as matched by matchCounter). Katakana units keep
// their katakana. Two readings are joined with '・' (1日: ついたち・いちにち).
bool read(uint64_t number, std::string_view counter, std::string& out);

}  // namespace CounterReading
