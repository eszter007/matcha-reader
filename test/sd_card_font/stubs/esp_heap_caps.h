#pragma once
// Host stub: SdCardFont.cpp reports the heap state alongside a failed interval allocation, which
// is device diagnostics. The host allocator never fails here, so the numbers are unused -- they
// only have to compile. Mirrors test/inflate_stream/stubs/esp_heap_caps.h.
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_8BIT = 4;
constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_INTERNAL = 2;
inline size_t heap_caps_get_free_size(uint32_t) { return 0; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
