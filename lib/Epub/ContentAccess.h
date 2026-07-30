#pragma once

// Optional alternative source for container items.
//
// Epub normally reads every item straight out of the ZIP. These functions let a
// build interpose a different source for some or all items, without Epub
// knowing anything about how that source works.
//
// This build supplies no such source: open() succeeds with a null handle and
// handles() is always false, so every item takes the ZIP path and behaviour is
// identical to having no hook. Handle is intentionally opaque -- it is declared
// here and defined only by an implementation that provides one.

#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace contentaccess {

class Handle;

struct HandleDeleter {
  // Out-of-line so unique_ptr never needs Handle to be a complete type.
  void operator()(Handle* handle) const;
};

using HandlePtr = std::unique_ptr<Handle, HandleDeleter>;

// Prepares the source for `containerPath`.
//   true  -> carry on; *out may be null, meaning "no alternative source"
//   false -> refuse the open; *err holds a user-presentable reason
bool open(const std::string& containerPath, HandlePtr* out, std::string* err);

// Should this item be read through the handle instead of the ZIP path?
bool handles(const HandlePtr& handle, const std::string& itemPath);

// Reads one item into a malloc'd buffer the caller owns. nullptr on failure.
uint8_t* readToBytes(const HandlePtr& handle, const std::string& itemPath, size_t* size, bool trailingNullByte);

// Reads one item, writing it to `out`.
bool readToStream(const HandlePtr& handle, const std::string& itemPath, Print& out);

}  // namespace contentaccess
