#!/usr/bin/env python3
"""
Pack shared `storage` partition binary (0x6A0000, size 1.375MB = 1441792 bytes)
for zero-copy `esp_partition_mmap` access by both `factory` (Launcher) and
`ota_1` (NevoFlux firmware).

Header layout (32 bytes, Big-Endian):
  0..4   magic "NFST"
  4..8   version = 1
  8..12  cjk_font_offset
  12..16 cjk_font_size
  16..20 vad_model_offset
  20..24 vad_model_size
  24..32 reserved = 0
"""

from __future__ import annotations

import argparse
import pathlib
import struct

STORAGE_MAGIC = b"NFST"
STORAGE_VERSION = 1
STORAGE_HEADER_SIZE = 32
STORAGE_MAX_BYTES = 0x160000  # 1,441,792 bytes (1.375 MB)


def build_storage_image(font_bytes: bytes, vad_bytes: bytes) -> bytes:
    font_offset = STORAGE_HEADER_SIZE
    font_size = len(font_bytes)
    # 4-byte align VAD model offset
    vad_offset = (font_offset + font_size + 3) & ~3
    vad_size = len(vad_bytes)
    total = vad_offset + vad_size
    if total > STORAGE_MAX_BYTES:
        raise ValueError(f"Storage image size {total} exceeds partition size {STORAGE_MAX_BYTES}")

    header = struct.pack(
        ">4sIIIII8x",
        STORAGE_MAGIC,
        STORAGE_VERSION,
        font_offset,
        font_size,
        vad_offset,
        vad_size,
    )
    pad = b"\xFF" * (vad_offset - (font_offset + font_size))
    return header + font_bytes + pad + vad_bytes


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack NevoFlux Pendant storage partition image")
    parser.add_argument("--font", type=pathlib.Path, help="Path to 20px CJK bitmap font binary")
    parser.add_argument("--vad", type=pathlib.Path, help="Path to ESP-SR VAD model binary")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="Output storage.bin path")
    args = parser.parse_args()

    font_data = args.font.read_bytes() if args.font and args.font.exists() else b"CJK_FONT_20PX_STUB"
    vad_data = args.vad.read_bytes() if args.vad and args.vad.exists() else b"ESP_SR_VAD_MODEL_STUB"

    img = build_storage_image(font_data, vad_data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(img)
    print(f"[OK] Generated storage image ({len(img)} bytes) -> {args.out}")


if __name__ == "__main__":
    main()
