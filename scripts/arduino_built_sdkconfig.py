"""CONFIG_* values the Arduino SDK libraries were last built with.

pioarduino restores the stock sdkconfig text files after a custom_sdkconfig rebuild, so
only the memory variant's generated include/sdkconfig.h matches the built archives. The
variant is chosen in the same order as builder/frameworks/espidf.py in platform 55.03.3xx.
"""
from pathlib import Path


def memory_variant_dir(env, libs_mcu):
    board = env.BoardConfig()
    memory = env.GetProjectOption("board_build.memory_type", "") or (
        board.get("build.arduino.memory_type", "")
        or board.get("build.memory_type", "")
        or f"{board.get('build.flash_mode', '') or 'dio'}_qspi"
    )
    speed = env.GetProjectOption("board_build.f_flash", "") or board.get("build.f_flash", "40000000L")
    try:
        mhz = int(str(speed).strip().rstrip("Ll"))
        if mhz >= 1_000_000:
            mhz //= 1_000_000
        speed_dir = libs_mcu / f"{memory}_{mhz}m"
        if speed_dir.is_dir():
            return speed_dir
    except ValueError:
        pass
    return libs_mcu / memory


def built_sdkconfig(env, libs_mcu):
    header = memory_variant_dir(env, libs_mcu) / "include" / "sdkconfig.h"
    values = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        parts = line.split(None, 2)
        if len(parts) >= 2 and parts[0] == "#define" and parts[1].startswith("CONFIG_"):
            values[parts[1]] = parts[2].strip() if len(parts) == 3 else ""
    return values
