#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// A .matcha manga volume: the converter's folder packed into one ZIP whose entries are all stored
// (not compressed), so an entry's bytes sit in the file as-is and are read through a window onto
// it -- no inflate. Renamed to .zip it unzips back into the folder.
//
// The first entry is matcha.idx: a table of every entry's name hash, data offset and size, sorted
// by hash, so a lookup is a binary search of ~log2(n) small reads instead of a walk of the ZIP's
// central directory. Format (little-endian): "MTCH", u32 version, u32 count, then count records
// of {u32 fnv1a(name), u32 dataOffset, u32 size}. A directory ("panels/") has size 0xFFFFFFFF.
// scripts/pack_matcha.py writes it, and refuses names whose hashes collide.
namespace matcha {

constexpr char kExtension[] = ".matcha";
constexpr uint32_t kDirectorySize = 0xFFFFFFFFu;

struct Entry {
  uint32_t offset = 0;
  uint32_t size = 0;
  bool isDirectory() const { return size == kDirectorySize; }
};

// Splits "/a/vol.matcha/page_0001.jpg" into the container "/a/vol.matcha" and the entry
// "page_0001.jpg". False for any path without a ".matcha/" component -- folder volumes included.
bool splitPath(const char* path, std::string& container, std::string& entry);

// Reads n bytes at an absolute offset of the open container; false on a short read. The storage
// layer supplies it, so this code stays free of any file API.
using ReadAt = bool (*)(void* ctx, uint32_t offset, void* buf, size_t n);

// Looks `entry` up in a container. A trailing '/' is ignored, so "panels" finds "panels/".
bool findEntry(ReadAt readAt, void* ctx, const std::string& entry, Entry& out);

}  // namespace matcha
