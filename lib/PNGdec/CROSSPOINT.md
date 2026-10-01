# PNGdec (vendored)

bitbank2/PNGdec 1.1.6 (Apache-2.0, see LICENSE), copied in from the PlatformIO registry
release. Kept out of clang-format and cppcheck so it stays diffable against upstream.

Local change: the scanline buffer `ucPixels` is no longer a fixed `PNG_MAX_BUFFERED_PIXELS`
array inside `PNGIMAGE` but a caller-owned pointer set with `PNG::setRowBuffer()` after
`open()`. `DecodePNG` refuses to run (`PNG_NO_BUFFER`) when it is missing or smaller than two
aligned scanlines. This takes ~16 KB out of the decoder object, so it fits in the lent
framebuffer on low-heap devices (see `PngToFramebufferConverter`).

