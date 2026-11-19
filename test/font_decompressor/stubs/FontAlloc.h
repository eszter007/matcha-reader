#pragma once

#include <cstddef>

// Declarations only: FontDecompressorTest defines these to count allocations.
extern "C" {
void* fiFontMalloc(size_t size);
void* fiFontRealloc(void* ptr, size_t size);
void fiFontFree(void* ptr);
}
