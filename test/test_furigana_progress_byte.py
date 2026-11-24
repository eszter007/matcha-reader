"""Check the progress.bin furigana byte: old values read as before and new ones round-trip."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r"""
#include <cassert>
#include <cstdint>
#include "activities/reader/FuriganaProgressByte.h"
using namespace EpubReaderUtils;
int main() {
  int8_t on = 0, size = 0;
  // Bytes written before sizes existed.
  unpackFurigana(0xFF, on, size); assert(on == -1 && size == -1);
  unpackFurigana(0x00, on, size); assert(on == 0 && size == -1);
  unpackFurigana(0x01, on, size); assert(on == 1 && size == -1);
  assert(static_cast<uint8_t>(packFurigana(-1, -1)) == 0xFF);
  assert(static_cast<uint8_t>(packFurigana(0, -1)) == 0x00);
  assert(static_cast<uint8_t>(packFurigana(1, -1)) == 0x01);
  // Every on/off choice with every size round-trips.
  for (int o = -1; o <= 1; o++) {
    for (int s = -1; s <= 3; s++) {
      unpackFurigana(static_cast<uint8_t>(packFurigana(o, s)), on, size);
      assert(on == o && size == s);
    }
  }
  // A high nibble no size uses reads as no size override.
  for (int high = 5; high <= 0xE; high++) {
    unpackFurigana(static_cast<uint8_t>((high << 4) | 1), on, size);
    assert(on == 1 && size == -1);
  }
  return 0;
}
"""

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "byte.cpp"
    exe = Path(tmp) / "byte"
    src.write_text(SOURCE)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
print("Furigana progress byte checks passed")
