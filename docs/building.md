# Building from source

```bash
git clone --recursive https://github.com/eszter007/matcha-reader.git
cd matcha-reader
git submodule update --init --recursive   # if you cloned without --recursive
pio run              # build
pio run -t upload    # flash
```

Same PlatformIO setup as upstream. Development notes are in [CLAUDE.md](../CLAUDE.md), the on-card cache formats in [file-formats.md](file-formats.md).

## Running without a device

The [simulator](https://github.com/eszter007/crosspoint-simulator-ios) builds Matcha from these same sources and renders the e-ink panel for you. It runs two ways:

- **Desktop** (macOS or Linux/WSL) through PlatformIO, in an SDL2 window.
- **iPhone**, as an app built with CMake and Xcode, with the panel taking real touch input. macOS only — there is no way to build an iOS app from Linux or Windows.

It is not limited to one board. `-DSIMULATOR_DEVICE=` selects the target, defaulting to `x4pro`, with `x4`, `x3`, `x4classic`, `sticky` and `papermono` matching the PlatformIO envs, and `-DSIMULATOR_DISPLAY=uc8179|uc8279` overriding the panel controller. That makes it the practical way to check a change on hardware you do not own — the touch and Home-key boards in particular.

From the simulator checkout, point it at this repository. The path must be absolute — a relative one resolves against `ios/` rather than the simulator's root and fails with "No firmware at ...":

```bash
cmake -S ios -B build-matcha -DCROSSPOINT_FIRMWARE_ROOT="$HOME/Projects/matcha-reader"
cmake --build build-matcha
```
