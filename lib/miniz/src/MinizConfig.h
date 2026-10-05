/* CrossPoint only needs miniz's low-level streaming inflate (tinfl). The
 * archive, deflate, stdio, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

// The ESP32 mask ROM exports tinfl_* at fixed addresses via DIRECT linker
// script assignments (e.g. "tinfl_decompress = 0x...;" in the ROM .ld),
// which override object-file definitions -- without these renames the
// firmware silently binds to the ROM's 2021 build (TINFL_LESS_MEMORY, a
// different tinfl_decompressor layout) and corrupts inflate state on real
// data. Rename so the linker can never capture them. The prefix is
// crosspoint_ (NOT freeink_) so a future branch that links FreeInkBook's
// identically-renamed copy does not collide.
#define tinfl_decompress crosspoint_tinfl_decompress
#define tinfl_decompress_mem_to_heap crosspoint_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem crosspoint_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback crosspoint_tinfl_decompress_mem_to_callback
#define mz_crc32 crosspoint_mz_crc32
#define mz_adler32 crosspoint_mz_adler32
#define mz_free crosspoint_mz_free

// miniz's own allocations (mz_inflateInit2's ~41KB inflate state, used by the protected-content
// reader) take the lent framebuffer when one is lent, as InflateStream does: on a fragmented heap
// there is rarely a free block that size. See MinizAlloc.cpp.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* crosspoint_mz_malloc(size_t bytes);
void crosspoint_mz_release(void* p);
void* crosspoint_mz_realloc(void* p, size_t bytes);
#ifdef __cplusplus
}
#endif
#define MZ_MALLOC(x) crosspoint_mz_malloc(x)
#define MZ_FREE(x) crosspoint_mz_release(x)
#define MZ_REALLOC(p, x) crosspoint_mz_realloc(p, x)

// Include the vendored miniz by relative path: ESP-IDF ships a ROM miniz.h
// with the SAME include guard but a different (TINFL_LESS_MEMORY) struct
// layout -- resolving <miniz.h> through the platform include path would
// silently compile against the wrong structures.
#include "../third_party/miniz.h"
