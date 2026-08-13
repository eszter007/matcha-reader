// Default: no alternative item source. Every item is read from the ZIP.
//
// This lives in src/ rather than lib/ so a build that provides its own
// implementation can exclude this file outright. Files under lib/ are archived,
// where overriding an already-resolved definition is unreliable; excluding a
// source file is deterministic, and a mistake shows up as a duplicate symbol at
// link time rather than silently falling back to these no-ops.

#include <ContentAccess.h>

namespace contentaccess {

void HandleDeleter::operator()(Handle*) const {}

// Returns true. That means "nothing to do, carry on" -- NOT "failed". The call
// site treats false as a refusal to open the book, so returning false here
// would make every book fail to load.
bool open(const std::string&, HandlePtr* out, std::string* err) {
  out->reset();
  err->clear();
  return true;
}

bool handles(const HandlePtr&, const std::string&) { return false; }

uint8_t* readToBytes(const HandlePtr&, const std::string&, size_t*, bool) { return nullptr; }

bool readToStream(const HandlePtr&, const std::string&, Print&) { return false; }

}  // namespace contentaccess
