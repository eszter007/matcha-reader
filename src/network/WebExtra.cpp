// Default: no extra routes.
//
// A build that registers its own excludes this file and compiles its own in
// place of it, the same way src/ContentAccess.cpp is replaced. Excluding a
// source file is deterministic; forgetting to exclude it is a duplicate symbol
// at link time rather than a silent fallback to this empty body.

#include "WebExtra.h"

namespace webextra {

void registerRoutes(WebServer*) {}

}  // namespace webextra
