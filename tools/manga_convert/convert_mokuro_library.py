#!/usr/bin/env python3
"""Convert a library of mokuro-processed manga into .matcha books, one per volume.

A library is a folder of series. Each series is either a .zip/.cbz archive or a folder, holding one
`<volume>.mokuro` file per volume beside a `<volume>/` folder of page images -- the layout mokuro
writes (a volume named `<volume>.mo.mokuro` may keep its images in `<volume>/`). Every volume
becomes `<output-dir>/<series>/<volume>.matcha` through convert_manga.py, which takes the text and
line boxes from the .mokuro file instead of reading the pages again.

Volumes that already have a .matcha are skipped, so an interrupted run resumes where it stopped. A
volume is written under a hidden name and renamed only once it is complete, so the device never
sees half a book even when the output is the SD card itself.

Usage:
    python3 convert_mokuro_library.py --library ~/Documents/mokuro/Mokuro --list
    python3 convert_mokuro_library.py --library ~/Documents/mokuro/Mokuro \\
        --output-dir /run/media/$USER/SDCARD/manga --series "Aho Girl" --series yotsuba \\
        -- --yonkoma

Arguments after `--` go to convert_manga.py unchanged (e.g. --mono, --trim-margins, --yonkoma).
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

CONVERTER = Path(__file__).resolve().parent / "convert_manga.py"
MOKURO_SUFFIX = ".mokuro"
ARCHIVE_SUFFIXES = {".zip", ".cbz"}
# Characters FAT/exFAT reject in a file name, plus control characters.
_UNSAFE_NAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
# Bytes, not characters: ext4 allows 255 per name, and ".<name>.partial.matcha.tmp" is added.
MAX_NAME_BYTES = 180
# Set by this script itself; a second value from the pass-through arguments would conflict.
RESERVED_PASSTHROUGH = {"--legacy", "--max-pages", "--device", "--x3", "--x4", "--input", "--output-dir"}


@dataclass
class Volume:
    series: str
    name: str        # the .mokuro file's stem, without a ".mo" before .mokuro
    source: Path     # the archive, or the .mokuro file in a series folder
    member: str = ""  # path of the .mokuro file inside the archive


def safe_name(name: str) -> str:
    """A file name every SD card file system accepts."""
    cleaned = _UNSAFE_NAME.sub("_", name)
    cleaned = cleaned.encode("utf-8")[:MAX_NAME_BYTES].decode("utf-8", "ignore")
    return cleaned.strip().rstrip(". ") or "untitled"


def output_paths(volumes: list[Volume], output_dir: Path) -> dict[int, Path]:
    """Output file per volume, by index. Names that collide once cleaned, cut to length or
    compared without case (FAT and exFAT ignore it) get " (2)", " (3)"... in library order, so two
    volumes never share a file or a staging folder."""
    paths: dict[int, Path] = {}
    taken: set[str] = set()
    for i, v in enumerate(volumes):
        folder = output_dir / safe_name(v.series)
        base = safe_name(v.name)
        name, n = base, 1
        while str(folder / name).casefold() in taken:
            n += 1
            name = f"{base} ({n})"
        taken.add(str(folder / name).casefold())
        paths[i] = folder / (name + ".matcha")
    return paths


def _safe_member(name: str) -> bool:
    norm = name.replace("\\", "/")
    parts = norm.split("/")
    # macOS archive litter: resource forks that only look like volumes.
    if "__MACOSX" in parts or parts[-1].startswith("._"):
        return False
    return not norm.startswith("/") and ".." not in parts and not (len(norm) > 1 and norm[1] == ":")


def find_volumes(library: Path, unreadable: list[str]) -> list[Volume]:
    """Every volume in the library; archives that cannot be opened are named in `unreadable`."""
    volumes = []
    for entry in sorted(library.iterdir(), key=lambda p: p.name.lower()):
        if entry.name.startswith("."):
            continue
        if entry.is_file() and entry.suffix.lower() in ARCHIVE_SUFFIXES:
            try:
                with zipfile.ZipFile(entry) as zf:
                    members = [n for n in zf.namelist() if n.lower().endswith(MOKURO_SUFFIX) and _safe_member(n)]
            except (zipfile.BadZipFile, OSError):
                unreadable.append(entry.name)
                continue
            for member in sorted(members):
                volumes.append(Volume(entry.stem, image_dirs(Path(member).stem)[-1], entry, member))
        elif entry.is_dir():
            for mokuro in sorted(entry.rglob("*" + MOKURO_SUFFIX)):
                volumes.append(Volume(entry.name, image_dirs(mokuro.stem)[-1], mokuro))
    return volumes


def matches(series: str, patterns: list[str]) -> bool:
    if not patterns:
        return True
    lowered = series.lower()
    return any(fnmatch.fnmatch(lowered, p.lower()) if any(c in p for c in "*?[") else p.lower() in lowered
               for p in patterns)


def image_dirs(stem: str) -> list[str]:
    """Folder names a volume's images may sit in: the .mokuro file's stem, and for
    `<volume>.mo.mokuro` (as some mokuro setups name it) also `<volume>`."""
    return [stem, stem[:-3]] if stem.endswith(".mo") else [stem]


def extract_volume(volume: Volume, work_dir: Path) -> Path:
    """Extract one volume's .mokuro file and image folder; returns the extracted .mokuro path."""
    prefixes = tuple(d + "/" for d in image_dirs(volume.member[: -len(MOKURO_SUFFIX)]))
    with zipfile.ZipFile(volume.source) as zf:
        for info in zf.infolist():
            name = info.filename
            if info.is_dir() or not _safe_member(name):
                continue
            if name != volume.member and not name.replace("\\", "/").startswith(prefixes):
                continue
            target = work_dir.joinpath(*name.replace("\\", "/").split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
    return work_dir.joinpath(*volume.member.split("/"))


def _discard(staging: Path, packed: Path) -> None:
    shutil.rmtree(staging, ignore_errors=True)
    for leftover in (packed, packed.with_name(packed.name + ".tmp")):
        leftover.unlink(missing_ok=True)


def convert(volume: Volume, final: Path, device: str, extra: list[str]) -> tuple[Volume, bool, str]:
    # Hidden while in progress; the converter packs <dir> into <dir>.matcha.
    staging = final.parent / ("." + final.stem + ".partial")
    packed = staging.with_name(staging.name + ".matcha")
    try:
        final.parent.mkdir(parents=True, exist_ok=True)
        _discard(staging, packed)
        with tempfile.TemporaryDirectory(prefix="mokuro_volume_") as tmp:
            try:
                mokuro = volume.source if not volume.member else extract_volume(volume, Path(tmp))
            except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError) as e:
                # The directory read fine but this volume's data cannot be: usually a download with
                # pieces still missing.
                _discard(staging, packed)
                return volume, False, f"damaged archive ({e})"
            cmd = [sys.executable, str(CONVERTER), "--input", str(mokuro), "--output-dir", str(staging),
                   "--device", device, *extra]
            result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0 or not packed.is_file():
            _discard(staging, packed)
            tail = (result.stderr or result.stdout).strip().splitlines()[-5:]
            return volume, False, "\n    ".join(tail)
        os.replace(packed, final)
        return volume, True, f"{final.stat().st_size / 1e6:.1f} MB"
    except Exception as e:  # one volume's failure must not stop the others or the report
        _discard(staging, packed)
        return volume, False, repr(e)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True, type=Path, help="Folder of series archives or series folders")
    parser.add_argument("--output-dir", type=Path, help="Where <series>/<volume>.matcha go, e.g. the SD card's manga/")
    parser.add_argument("--series", action="append", default=[],
                        help="Only series whose name contains this text (case-insensitive) or matches this glob. "
                             "Repeat for several.")
    parser.add_argument("--list", action="store_true", help="List the matching series and their volume counts, then stop")
    parser.add_argument("--device", default="x4pro", help="Screen to fit pages to (convert_manga.py --device)")
    parser.add_argument("--jobs", type=int, default=max(1, min(4, (os.cpu_count() or 2) // 2)),
                        help="Volumes converted at once")
    parser.add_argument("--force", action="store_true", help="Convert again even when the .matcha exists")
    args, extra = parser.parse_known_args()
    if extra and extra[0] == "--":
        extra = extra[1:]
    # By prefix too: the converter's parser accepts "--max" for --max-pages.
    reserved = sorted({name for name in (a.split("=", 1)[0] for a in extra)
                       if name.startswith("--") and len(name) > 2
                       and any(option.startswith(name) for option in RESERVED_PASSTHROUGH)})
    if reserved:
        # --legacy writes a folder that cannot be finished atomically, and --max-pages would leave a
        # short book that a later run takes as done.
        sys.exit(f"{', '.join(reserved)} cannot be passed through: this script sets it or cannot finish with it. "
                 "Use --device here, or convert_manga.py for one volume.")

    unreadable: list[str] = []
    library = find_volumes(args.library.expanduser(), unreadable)
    volumes = [v for v in library if matches(v.series, args.series)]
    unreadable = [name for name in unreadable if matches(Path(name).stem, args.series)]
    if unreadable:
        # A zip without its central directory -- typically a download that never finished, its
        # tail still the zeros the client preallocated.
        shown = unreadable if args.list else unreadable[:5]
        more = "" if len(shown) == len(unreadable) else f", and {len(unreadable) - len(shown)} more (see --list)"
        print(f"{len(unreadable)} archive(s) could not be opened (unfinished downloads?): "
              + ", ".join(shown) + more, file=sys.stderr)
    if args.list:
        counts: dict[str, int] = {}
        for v in volumes:
            counts[v.series] = counts.get(v.series, 0) + 1
        for series, count in counts.items():
            print(f"{count:4d}  {series}")
        print(f"{len(volumes)} volumes in {len(counts)} series")
        return
    if not args.output_dir:
        sys.exit("--output-dir is required unless --list is given")
    if not volumes:
        sys.exit("No volumes matched.")

    # Numbered over the whole library, so a volume keeps its name whatever --series selects.
    all_paths = output_paths(library, args.output_dir.expanduser())
    paths = {id(v): all_paths[i] for i, v in enumerate(library)}
    todo = [v for v in volumes if args.force or not paths[id(v)].exists()]
    print(f"{len(volumes)} volumes matched, {len(volumes) - len(todo)} already converted, {len(todo)} to go")

    failed = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(convert, v, paths[id(v)], args.device, extra) for v in todo]
        for done, future in enumerate(as_completed(futures), 1):
            volume, ok, detail = future.result()
            status = "ok  " if ok else "FAIL"
            print(f"[{done}/{len(todo)}] {status} {volume.series} / {volume.name}: {detail}", flush=True)
            failed += not ok
    if failed:
        sys.exit(f"{failed} volume(s) failed. Fix or re-download their archives and run again: converted volumes "
                 "are skipped.")


if __name__ == "__main__":
    main()
