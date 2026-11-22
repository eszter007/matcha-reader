"""Match the C3 controller archive to the rebuilt SDK's flash-only setting.

PioArduino 55.03.3xx retains the packaged -lbtdm_app linker list after a custom
SDK rebuild. ESP-IDF selects libbtdm_app_flash.a for this configuration. Mixing
the flash-only header with the other archive makes controller init fail.
"""
import importlib.util
from pathlib import Path


_FLASH_ONLY_BT_ROM_SCRIPTS = frozenset(
    {
        "esp32c3.rom.bt_funcs.ld",
        "esp32c3.rom.eco3_bt_funcs.ld",
        "esp32c3.rom.eco7_bt_funcs.ld",
    }
)


def filter_flash_only_bt_linkflags(linkflags):
    """Remove only BT ROM linker-script pairs omitted by flash-only IDF.

    PioArduino's C3 linker list is a flat sequence of ``-T``/filename pairs.
    The matching ESP-IDF build omits the three BT ROM scripts above when
    ``CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`` is enabled.  Keep every other flag,
    including the main ``esp32c3.rom.ld`` script, and tolerate malformed or
    non-list values from a host fixture without indexing past the end.
    """
    if linkflags is None:
        return []
    if isinstance(linkflags, str):
        flags = [linkflags]
    else:
        try:
            flags = list(linkflags)
        except TypeError:
            flags = [linkflags]

    filtered = []
    index = 0
    while index < len(flags):
        candidate = flags[index + 1] if index + 1 < len(flags) else None
        if flags[index] == "-T" and isinstance(candidate, str) and candidate in _FLASH_ONLY_BT_ROM_SCRIPTS:
            index += 2
            continue
        filtered.append(flags[index])
        index += 1
    return filtered


def configure_controller(env):
    # pio check only dumps include paths and defines; nothing links, and the SDK libraries may
    # not have been rebuilt for this environment.
    if env.IsIntegrationDump():
        return
    if env.BoardConfig().get("build.mcu") != "esp32c3":
        return
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    requested = env.GetProjectOption("custom_sdkconfig", "")
    if "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y" not in requested.split():
        return
    platform = env.PioPlatform()
    framework = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))
    helper = Path(env.subst("$PROJECT_DIR")) / "scripts" / "arduino_built_sdkconfig.py"
    spec = importlib.util.spec_from_file_location("crosspoint_built_sdkconfig", helper)
    built_config = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(built_config)
    if built_config.built_sdkconfig(env, framework / "esp32c3").get("CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY") != "1":
        raise RuntimeError("BLE controller configuration does not match the rebuilt C3 SDK")
    idf = platform.get_package_dir("framework-espidf")
    if not idf:
        raise RuntimeError("The matching ESP-IDF package is required for the flash-only BLE controller")
    archives = Path(idf) / "components/bt/controller/lib_esp32c3_family/esp32c3"
    if not (archives / "libbtdm_app_flash.a").is_file():
        raise RuntimeError("ESP-IDF flash-only BLE controller archive is missing")
    libs = list(env["LIBS"])
    if not any(str(lib) in ("btdm_app", "-lbtdm_app") for lib in libs):
        raise RuntimeError("Unsupported PioArduino BLE controller linker list")
    env.Replace(LIBS=["-lbtdm_app_flash" if str(lib) in ("btdm_app", "-lbtdm_app") else lib for lib in libs])
    env.Append(LIBPATH=[str(archives)])
    env.Replace(LINKFLAGS=filter_flash_only_bt_linkflags(env.get("LINKFLAGS", [])))


try:
    Import("env")  # noqa: F821
except NameError:
    pass
else:
    configure_controller(env)  # noqa: F821
