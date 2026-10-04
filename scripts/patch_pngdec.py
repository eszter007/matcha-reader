"""
PlatformIO post: script: apply CrossPoint's PNGdec patch via `git apply`.

The patch in `scripts/pngdec_patches/` moves PNGdec's scanline buffer out of
the decoder object into a caller-owned allocation (see the file header). It
targets PNGdec 1.1.6 only, with the same guards as patch_sdfat.py:
  * the target must live under this project's `.pio/libdeps/<env>/`
  * `library.properties` must say version 1.1.6
  * each target file must hash to the reviewed upstream or patched bytes
Any mismatch fails the build. Upgrading PNGdec therefore requires
re-reviewing the patch and hashes.

The whole patch is checked before any file is changed, and an already-patched
tree is left untouched so incremental builds do not recompile PNGdec.
"""

import hashlib
import os
from pathlib import Path
import subprocess

PATCH = "0001-caller-owned-row-buffer.patch"
# Reviewed upstream and patched bytes per file. A dependency upgrade requires re-review.
TARGETS = (
    ("src/PNGdec.h",
     "f01b12dea5d14df02aa0ae7b9e24c415b7d40879221e90ab644d7215a3c2a37f",
     "cc0129c0a9c4b7e71cc5c39edc90d70fcff092046c5af513a00591033fd85e0a"),
    ("src/PNGdec.cpp",
     "2b534fbac2684848104f5eecb21ebf99402efc47659c686ecaf79e3b604a9f5c",
     "90c411c0d7e20eda4abafedc1b244e5f38b184c758a7681fbb094326d1ee2472"),
    ("src/png.inl",
     "8008f43fb74e578df3651791da4fb4ac74cb87f946ec6910aeab703fa5b5ea65",
     "4331a9a790225d9cf2cd6b6e57f4257a70c21094ffb88df84d487976b417c3ff"),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_within(path, root):
    # Path.is_relative_to() needs Python 3.9; the documented minimum is 3.8.
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def apply_patch(project_dir, dependency_dir):
    project = Path(project_dir).resolve()
    dependency = Path(dependency_dir).resolve()
    if not is_within(dependency, project / ".pio" / "libdeps"):
        raise RuntimeError("PNGdec must be inside this project's .pio/libdeps")
    properties = dependency / "library.properties"
    files = [properties] + [dependency / target[0] for target in TARGETS]
    for path in files:
        if not is_within(path.resolve(), dependency) or not path.is_file():
            raise RuntimeError("Missing or escaping PNGdec patch target: " + path.name)
    if "version=1.1.6" not in properties.read_text().splitlines():
        raise RuntimeError("PNGdec patch requires version 1.1.6")

    states = []
    for relative, upstream, patched in TARGETS:
        current = digest(dependency / relative)
        if current == upstream:
            states.append("upstream")
        elif current == patched:
            states.append("patched")
        else:
            # Usually a copy patched by an older revision of the patch.
            raise RuntimeError("Unrecognized PNGdec source: " + relative +
                               "; delete .pio/libdeps/*/PNGdec and rebuild")
    if all(state == "patched" for state in states):
        return
    if any(state == "patched" for state in states):
        raise RuntimeError("PNGdec is partially patched; delete .pio/libdeps/*/PNGdec and rebuild")

    # Archive dependencies must not discover the enclosing application Git repo.
    process_env = {key: os.environ[key] for key in ("HOME", "PATH", "TMPDIR", "LANG") if key in os.environ}
    process_env.update(GIT_CEILING_DIRECTORIES=str(dependency.parent),
                       GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
    patch = project / "scripts" / "pngdec_patches" / PATCH
    result = subprocess.run(["git", "apply", "--check", str(patch)], cwd=dependency,
                            env=process_env, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("PNGdec patch does not apply: " + PATCH + "\n" + result.stderr)
    subprocess.run(["git", "apply", str(patch)], cwd=dependency, env=process_env, check=True)
    for relative, _, patched in TARGETS:
        if digest(dependency / relative) != patched:
            raise RuntimeError("Unexpected patched PNGdec source: " + relative)
    print("Applied PNGdec patch: " + PATCH)


def patch_selected_dependency(env):
    # PioArduino's SDK-only pass clears lib_deps; the later app pass resolves them.
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    selected = [builder for builder in env.GetLibBuilders() if builder.name == "PNGdec"]
    if len(selected) != 1:
        raise RuntimeError("Expected exactly one selected PNGdec dependency")
    dependency = Path(selected[0].path).resolve()
    environment_root = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    if not is_within(dependency, environment_root.resolve()):
        raise RuntimeError("PNGdec is outside the selected environment's dependencies")
    apply_patch(env["PROJECT_DIR"], dependency)


if "Import" in globals():
    Import("env")  # noqa: F821 -- supplied by SCons
    patch_selected_dependency(env)  # noqa: F821
