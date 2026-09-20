#pragma once

#include <string>

// The folder the firmware keeps its own state in: reading statistics, the clock stash, the
// translate hand-off, the API key. Two spellings are accepted -- "/.system" and "/system" -- the
// same way the font registry takes /.fonts beside /fonts and the dictionaries take
// /.dictionaries beside /dictionaries. The dotted one keeps the folder out of the file browser
// (hidden entries are off by default) and is what gets created when neither exists.
//
// Lives in lib/hal because HalClock is one of the callers and cannot reach into src/.
namespace sdsystem {

// The resolved folder, without a trailing slash. Probed once, on the first call after the card is
// mounted; whichever spelling already exists wins, and "/.system" is created when neither does.
const char* dir();

// `dir()` + "/" + leaf. Returned by value: callers hold it for the length of one file operation,
// and a fixed buffer here would be one more thing to size wrongly.
std::string path(const char* leaf);

// For a file the USER puts in the folder rather than one the firmware writes (the API key): the
// resolved folder's copy if it has one, else the other spelling's. On a fresh card dir() creates
// "/.system", so someone following an on-screen "/system/" hint would otherwise never be found.
// Falls back to path(leaf) when neither has it, so the caller's not-found message still applies.
std::string findUserFile(const char* leaf);

}  // namespace sdsystem
