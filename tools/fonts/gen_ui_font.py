#!/usr/bin/env python3
"""
Generate the LVGL UI fonts compiled into components/bsp_143c (nf_font_20 / nf_font_32).

nf_font_20 is subset to printable ASCII plus every non-ASCII character that appears in
the firmware C sources, so it covers all on-screen Chinese strings. nf_font_32 (titles and
the NevoFlux mode label) only carries ASCII plus the characters of `mode_str = "..."`
labels, to keep the 1MB factory (Launcher) image within its partition. Re-run after adding new UI strings:

    python3 tools/fonts/gen_ui_font.py

Requirements: Node.js (npx lv_font_conv@1.5.2), fonttools (only to extract a face from a .ttc),
and a CJK font (default: Noto Sans CJK SC from the system font collection).
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "components" / "bsp_143c" / "src" / "fonts"
DEFAULT_FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
SIZES = (20, 32)
LARGE_SIZE = 32


def collect_chars() -> str:
    chars: set[str] = set()
    sources = list((REPO_ROOT / "apps").glob("*/main/*.c")) + list((REPO_ROOT / "components").glob("*/src/*.c"))
    for src in sources:
        chars.update(ch for ch in src.read_text(encoding="utf-8") if ord(ch) > 0x7E)
    return "".join(sorted(chars))


def collect_large_chars() -> str:
    chars: set[str] = set()
    for src in (REPO_ROOT / "apps").glob("*/main/*.c"):
        for label in re.findall(r'mode_str = "([^"]*)"', src.read_text(encoding="utf-8")):
            chars.update(ch for ch in label if ord(ch) > 0x7E)
    return "".join(sorted(chars))


def extract_face(ttc: pathlib.Path, face_name: str, out: pathlib.Path) -> None:
    from fontTools.ttLib import TTCollection  # type: ignore

    for font in TTCollection(str(ttc)):
        if font["name"].getDebugName(4) == face_name:
            font.save(str(out))
            return
    raise SystemExit(f"face {face_name!r} not found in {ttc}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", default=DEFAULT_FONT, help=".ttf/.otf, or .ttc together with --face")
    ap.add_argument("--face", default="Noto Sans CJK SC", help="face name inside a .ttc collection")
    args = ap.parse_args()

    symbols = collect_chars()
    large_symbols = collect_large_chars()
    print(f"{len(symbols)} non-ASCII glyphs: {symbols}")
    print(f"{len(large_symbols)} large-font glyphs: {large_symbols}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        font_path = pathlib.Path(args.font)
        if font_path.suffix.lower() == ".ttc":
            face = pathlib.Path(tmp) / "face.otf"
            extract_face(font_path, args.face, face)
            font_path = face
        for size in SIZES:
            size_symbols = large_symbols if size == LARGE_SIZE else symbols
            out = OUT_DIR / f"nf_font_{size}.c"
            cmd = [
                "npx", "--yes", "lv_font_conv@1.5.2",
                "--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size),
                "--format", "lvgl", "--lv-include", "lvgl.h",
                "--font", str(font_path), "--range", "0x20-0x7E", "--symbols", size_symbols,
                "-o", str(out),
            ]
            subprocess.run(cmd, check=True)
            print(f"wrote {out.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
