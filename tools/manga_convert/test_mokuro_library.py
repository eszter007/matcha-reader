#!/usr/bin/env python3
"""Self-check for convert_mokuro_library.py. Run: python3 test_mokuro_library.py
The helpers need nothing; the end-to-end case needs Pillow, as convert_manga.py does."""

import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from convert_mokuro_library import Volume, _safe_member, find_volumes, image_dirs, matches, output_paths, safe_name

HERE = Path(__file__).resolve().parent
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"  {detail}"))
    if not ok:
        failures += 1


check("series filter: text, glob, none", matches("Aho Girl", ["aho"]) and matches("Yotsuba to!", ["yotsuba*"])
      and not matches("Aho Girl", ["yotsuba"]) and matches("Anything", []))
check("names an SD card accepts", safe_name('Re:Zero? "A/B"') == "Re_Zero_ _A_B_" and safe_name("Vol. ") == "Vol",
      safe_name('Re:Zero? "A/B"'))
long = safe_name("あ" * 100)
check("long names cut at a character, under 180 bytes", len(long.encode("utf-8")) <= 180 and long == "あ" * 60)
check("archive litter and escaping paths are not volumes",
      not _safe_member("__MACOSX/Vol 1.mokuro") and not _safe_member("Series/._Vol 1.mokuro")
      and _safe_member("Series/Vol 1.mokuro") and not _safe_member("../Vol 1.mokuro"))
check("a .mo.mokuro volume may keep its images in <volume>/", image_dirs("Vol 1.mo") == ["Vol 1.mo", "Vol 1"]
      and image_dirs("Vol 1") == ["Vol 1"])

vols = [Volume("S", "Vol 1", Path("a.zip")), Volume("s", "VOL 1", Path("b.cbz")), Volume("S", "Vol?1", Path("c"))]
paths = [str(output_paths(vols, Path("/out"))[i]) for i in range(3)]
check("names that collide on a FAT card get numbered", len({n.casefold() for n in paths}) == 3
      and paths[0] == "/out/S/Vol 1.matcha" and paths[1].endswith("VOL 1 (2).matcha"), paths)


def page_png(path: Path, shade: int) -> None:
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (300, 420), "white")
    d = ImageDraw.Draw(img)
    d.rectangle([20, 20, 280, 200], outline="black", width=4, fill=(shade, shade, shade))
    d.rectangle([20, 220, 280, 400], outline="black", width=4)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def mokuro_json(volume: str, pages: list[str]) -> str:
    return json.dumps({"title": "Test Series", "volume": volume, "pages": [
        {"img_path": p, "img_width": 300, "img_height": 420, "blocks": [
            {"box": [40, 40, 80, 180], "vertical": True, "lines": ["テスト"], "lines_coords": [[[40, 40], [80, 40], [80, 180], [40, 180]]]}]}
        for p in pages]}, ensure_ascii=False)


def run(*args):
    return subprocess.run([sys.executable, str(HERE / "convert_mokuro_library.py"), *args], capture_output=True, text=True)


try:
    import PIL  # noqa: F401
    have_pil = True
except ImportError:
    have_pil = False

if not have_pil:
    print("skip end-to-end (no Pillow)")
else:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        lib = tmp / "library"
        # A series folder with one volume, and a series archive with two (one named .mo.mokuro).
        folder = lib / "Folder Series"
        page_png(folder / "Vol 1" / "001.png", 200)
        (folder / "Vol 1.mokuro").write_text(mokuro_json("Vol 1", ["001.png"]), encoding="utf-8")
        src = tmp / "zipsrc"
        page_png(src / "Vol 1" / "001.png", 180)
        page_png(src / "Vol 2" / "001.png", 160)
        page_png(src / "Vol 2" / "002.png", 140)
        with zipfile.ZipFile(lib / "Zip Series.cbz", "w") as zf:
            zf.writestr("Vol 1.mokuro", mokuro_json("Vol 1", ["001.png"]))
            zf.writestr("Vol 2.mo.mokuro", mokuro_json("Vol 2", ["001.png", "002.png"]))
            zf.writestr("__MACOSX/._Vol 1.mokuro", "junk")
            for f in sorted(src.rglob("*.png")):
                zf.write(f, f.relative_to(src).as_posix())
        (lib / "Broken.zip").write_bytes(b"PK\x03\x04" + b"\0" * 64)

        unreadable: list[str] = []
        found = find_volumes(lib, unreadable)
        check("volumes found in folders and archives, litter skipped",
              [(v.series, v.name) for v in found] == [("Folder Series", "Vol 1"), ("Zip Series", "Vol 1"), ("Zip Series", "Vol 2")],
              [(v.series, v.name) for v in found])
        check("an archive that cannot be opened is reported", unreadable == ["Broken.zip"], unreadable)

        listed = run("--library", str(lib), "--list")
        check("--list counts volumes per series", "3 volumes in 2 series" in listed.stdout, listed.stdout + listed.stderr)

        out = tmp / "sd" / "manga"
        first = run("--library", str(lib), "--output-dir", str(out), "--jobs", "2")
        books = sorted(str(p.relative_to(out)) for p in out.rglob("*.matcha"))
        check("one .matcha per volume, by series", first.returncode == 0 and books ==
              ["Folder Series/Vol 1.matcha", "Zip Series/Vol 1.matcha", "Zip Series/Vol 2.matcha"],
              f"{books}\n{first.stdout}\n{first.stderr}")
        leftovers = [p.name for p in out.rglob(".*")]
        check("nothing half-written is left behind", not leftovers, leftovers)
        with zipfile.ZipFile(out / "Zip Series" / "Vol 2.matcha") as zf:
            names = zf.namelist()
        check("the .mo.mokuro volume has both its pages", sum(n.startswith("page_") for n in names) == 2, names)

        again = run("--library", str(lib), "--output-dir", str(out))
        check("a second run skips what is done", "3 already converted, 0 to go" in again.stdout, again.stdout)
        some = run("--library", str(lib), "--output-dir", str(out), "--series", "zip", "--force")
        check("--series and --force convert just that series again", some.returncode == 0 and "[2/2]" in some.stdout,
              some.stdout + some.stderr)
        bad = run("--library", str(lib), "--output-dir", str(out), "--", "--max-pages", "1")
        check("options that would leave a short book are refused", bad.returncode != 0 and "--max-pages" in bad.stderr,
              bad.stderr)

if failures:
    raise SystemExit(f"\n{failures} failure(s)")
print("\nall tests passed")
