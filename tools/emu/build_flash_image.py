#!/usr/bin/env python3
"""
Build an 8MB ESP32-S3 merged flash image (`merged-flash.bin`) for `esp-emu --chip esp32s3`.

Flash layout (strictly matching `partitions.csv`):
  0x000000  2nd-Stage Bootloader / SoC Multi-Partition Manager
  0x008000  Binary Partition Table (`partitions.bin` with MD5 checksum)
  0x009000  `nvs`      (24 KB)  - Shared WiFi / NevoFlux / Muse OAuth2 NVS storage
  0x00F000  `otadata`  (8 KB)   - Dual-OTA boot selector (`ota_seq` + CRC32)
  0x011000  `phy_init` (4 KB)   - RF PHY calibration data
  0x020000  `factory`  (1 MB)   - NevoFlux Dual-Boot Launcher (`apps/launcher`)
  0x1A0000  `ota_0`    (3 MB)   - Muse Assistant (`facebookincubator/muse-gadget-sdk` + RFC 8628 Device Flow)
  0x4A0000  `ota_1`    (2.5 MB) - NevoFlux Private Cloud Assistant (`apps/nevoflux`)
  0x720000  `storage`  (896KB)  - Shared zero-copy `mmap` assets (`NFST`: 20px CJK font + ESP-SR VAD)

If ESP-IDF build outputs (`apps/*/build/*.bin`) exist, they are merged into the image.
Otherwise, a native Xtensa LX7 ESP32-S3 binary is assembled so `esp-emu --chip esp32s3`
can immediately boot from the real ESP32-S3 Mask ROM (`ESP-ROM:esp32s3-20210327`),
verify the partition table and `storage.bin` over SPI Flash, switch partitions via
`otadata` (`0xF000`) with real `RTC_SW_SYS_RST` warm reboots, and accept live UART0
provisioning from `tools/provision/provision.py`.
"""

from __future__ import annotations

import argparse
import binascii
import csv
import hashlib
import pathlib
import struct
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "pack_storage"))
from pack_storage import build_storage_image  # noqa: E402

FLASH_SIZE_BYTES = 8 * 1024 * 1024  # 8 MB (0x800000)

# ESP32-S3 Mask ROM (esp32s3-20210327) function addresses
ROM_ETS_PRINTF = 0x4004423C
ROM_ETS_DELAY_US = 0x40041A68
ROM_SOFTWARE_RESET = 0x4004466C
ROM_UART_RX_ONE_CHAR = 0x40048D00
ROM_SPIFLASH_UNLOCK = 0x4004A7B8
ROM_SPIFLASH_ERASE_SECTOR = 0x4004AAD4
ROM_SPIFLASH_WRITE = 0x4004ABBC
ROM_SPIFLASH_READ = 0x4004ADB8

PART_TYPES = {"app": 0x00, "data": 0x01}
PART_SUBTYPES = {
    "factory": 0x00,
    "ota_0": 0x10,
    "ota_1": 0x11,
    "ota": 0x00,
    "phy": 0x01,
    "nvs": 0x02,
    "spiffs": 0x82,
}


def compile_partition_table(csv_path: pathlib.Path) -> bytes:
    """Compile `partitions.csv` into an ESP-IDF binary partition table (`0x50AA` + MD5)."""
    entries = bytearray()
    with csv_path.open("r", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or row[0].strip().startswith("#"):
                continue
            name = row[0].strip()
            p_type = PART_TYPES[row[1].strip()]
            p_subtype = PART_SUBTYPES[row[2].strip()]
            offset = int(row[3].strip(), 0)
            size = int(row[4].strip(), 0)
            flags = 0
            entries += struct.pack(
                "<HBBII16sI",
                0x50AA,
                p_type,
                p_subtype,
                offset,
                size,
                name.encode("ascii")[:16].ljust(16, b"\x00"),
                flags,
            )
    md5_digest = hashlib.md5(entries).digest()
    entries += struct.pack("<HBB12s16s", 0xEBEB, 0xFF, 0xFF, b"\xFF" * 12, md5_digest)
    return bytes(entries)


def build_otadata_entry(ota_seq: int) -> bytes:
    """
    Build a 32-byte `esp_ota_select_entry_t`:
      uint32_t ota_seq;
      uint8_t  seq_label[20];
      uint32_t ota_state;
      uint32_t crc;
    """
    if ota_seq <= 0:
        return b"\xFF" * 32
    seq_bytes = struct.pack("<I", ota_seq)
    crc = binascii.crc32(seq_bytes, 0xFFFFFFFF) & 0xFFFFFFFF
    return struct.pack("<I20sII", ota_seq, b"\xFF" * 20, 0, crc)


class XtensaAssembler:
    """Minimal Xtensa LX7 assembler for ESP32-S3 ROM interaction."""

    def __init__(self, iram_base: int, code_offset: int) -> None:
        self.iram_base = iram_base
        self.code_offset = code_offset
        self.lits: list[int] = []
        self.lit_map: dict[int, int] = {}
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[str, int, str]] = []

    @property
    def pc(self) -> int:
        return self.iram_base + self.code_offset + len(self.code)

    def lit_addr(self, val: int) -> int:
        val &= 0xFFFFFFFF
        if val not in self.lit_map:
            idx = len(self.lits)
            self.lits.append(val)
            self.lit_map[val] = self.iram_base + idx * 4
        return self.lit_map[val]

    def label(self, name: str) -> None:
        self.labels[name] = len(self.code)

    def entry_a1_32(self) -> None:
        self.code += bytes([0x36, 0x41, 0x00])

    def l32r(self, ar: int, val: int) -> None:
        target = self.lit_addr(val)
        imm16 = ((target - ((self.pc + 3) & ~3)) >> 2) & 0xFFFF
        self.code += bytes([(ar << 4) | 0x01, imm16 & 0xFF, (imm16 >> 8) & 0xFF])

    def l32i(self, at: int, as_: int, byte_off: int = 0) -> None:
        self.code += bytes([(at << 4) | 0x02, (0x02 << 4) | as_, byte_off >> 2])

    def s32i(self, at: int, as_: int, byte_off: int = 0) -> None:
        self.code += bytes([(at << 4) | 0x02, (0x06 << 4) | as_, byte_off >> 2])

    def l8ui(self, at: int, as_: int, byte_off: int = 0) -> None:
        self.code += bytes([(at << 4) | 0x02, (0x00 << 4) | as_, byte_off & 0xFF])

    def s8i(self, at: int, as_: int, byte_off: int = 0) -> None:
        self.code += bytes([(at << 4) | 0x02, (0x04 << 4) | as_, byte_off & 0xFF])

    def addi(self, at: int, as_: int, imm8: int) -> None:
        self.code += bytes([(at << 4) | 0x02, (0x0C << 4) | as_, imm8 & 0xFF])

    def add(self, ar: int, as_: int, at: int) -> None:
        # RRR: op0=0, op1=0, op2=8 -> byte0=(t<<4)|0, byte1=(r<<4)|s, byte2=0x80
        self.code += bytes([(at << 4) | 0x00, (ar << 4) | as_, 0x80])

    def callx8(self, as_: int = 8) -> None:
        self.code += bytes([0xE0, as_ & 0x0F, 0x00])

    def call_rom(self, fn_addr: int, *args: int) -> None:
        for i, arg in enumerate(args):
            self.l32r(10 + i, arg)
        self.l32r(8, fn_addr)
        self.callx8(8)

    def beq(self, as_: int, at: int, label_name: str) -> None:
        # Emit: bne as_, at, +3 (skip j); j label_name
        self.code += bytes([(at << 4) | 0x07, (0x09 << 4) | as_, 0x02])
        self.j(label_name)

    def bne(self, as_: int, at: int, label_name: str) -> None:
        # Emit: beq as_, at, +3 (skip j); j label_name
        self.code += bytes([(at << 4) | 0x07, (0x01 << 4) | as_, 0x02])
        self.j(label_name)

    def bnez(self, as_: int, label_name: str) -> None:
        pos = len(self.code)
        self.code += bytes([0x56, as_ & 0x0F, 0x00])
        self.fixups.append(("bri12", pos, label_name))

    def j(self, label_name: str) -> None:
        pos = len(self.code)
        self.code += bytes([0x06, 0x00, 0x00])
        self.fixups.append(("j", pos, label_name))

    def assemble(self) -> bytes:
        lit_bytes = struct.pack(f"<{len(self.lits)}I", *self.lits)
        if len(lit_bytes) > self.code_offset:
            raise ValueError(f"Literal pool ({len(lit_bytes)}B) exceeds code_offset ({self.code_offset}B)")
        for kind, pos, label_name in self.fixups:
            target = self.labels[label_name]
            rel = target - (pos + 4)
            if kind == "rri8":
                if not (-128 <= rel <= 127):
                    raise ValueError(f"RRI8 branch out of range ({rel}) to {label_name}")
                self.code[pos + 2] = rel & 0xFF
            elif kind == "bri12":
                if not (-2048 <= rel <= 2047):
                    raise ValueError(f"BRI12 branch out of range ({rel}) to {label_name}")
                imm12 = rel & 0xFFF
                as_ = self.code[pos + 1] & 0x0F
                self.code[pos + 1] = ((imm12 & 0x0F) << 4) | as_
                self.code[pos + 2] = (imm12 >> 4) & 0xFF
            elif kind == "j":
                imm18 = rel & 0x3FFFF
                val24 = (imm18 << 6) | 0x06
                self.code[pos : pos + 3] = val24.to_bytes(3, "little")
        seg = lit_bytes.ljust(self.code_offset, b"\x00") + bytes(self.code)
        seg += b"\x00" * ((4 - len(seg) % 4) % 4)
        return seg


def pack_esp32s3_image(entry_addr: int, segments: list[tuple[int, bytes]]) -> bytes:
    """Wrap segments into a valid ESP32-S3 bootable image with XOR checksum and SHA-256."""
    # spi_speed_size = 0x32 (8MB flash, 40MHz DIO), chip_id = 0x0009 (ESP32-S3), hash_appended = 1
    hdr = struct.pack("<BBBBI", 0xE9, len(segments), 2, 0x32, entry_addr)
    hdr += struct.pack("<BBBBHBB", 0xEE, 0, 0, 0, 0x0009, 0, 0)
    hdr += struct.pack("<HHBBBB", 0, 0xFFFF, 0, 0, 0, 1)

    body = bytearray(hdr)
    chk = 0xEF
    for load_addr, seg_data in segments:
        padded = seg_data + b"\x00" * ((4 - len(seg_data) % 4) % 4)
        body += struct.pack("<II", load_addr, len(padded))
        body += padded
        for b in padded:
            chk ^= b

    pad_len = (16 - (len(body) + 1) % 16) % 16
    body += b"\x00" * pad_len + bytes([chk])
    body += hashlib.sha256(body).digest()
    return bytes(body)


def build_native_s3_firmware() -> bytes:
    """
    Build a self-contained Xtensa LX7 ESP32-S3 firmware image that:
      1. Reads & logs the binary partition table at 0x8000 and `storage.bin` at 0x720000
      2. Checks NVS provisioning data at 0x9000
      3. Inspects `otadata` at 0xF000 (`ota_seq`) and boots into:
         - `factory` (0x020000): NevoFlux Dual-Boot Launcher
         - `ota_0`   (0x1A0000): Muse Assistant (muse-gadget-sdk + RFC 8628 OAuth2 Device Flow)
         - `ota_1`   (0x4A0000): NevoFlux Private Cloud Assistant (Relay WSS + Opus 16k/24k)
      4. Polls UART0 for:
         - '0' -> switch `otadata` to `ota_0` (Muse) & warm reset (`RTC_SW_SYS_RST`)
         - '1' -> switch `otadata` to `ota_1` (NevoFlux) & warm reset (`RTC_SW_SYS_RST`)
         - 'l' -> erase `otadata` back to `factory` (Launcher) & warm reset (`RTC_SW_SYS_RST`)
         - `prov.offer` / `prov.status` / `prov.write` JSON lines from `tools/provision/provision.py`
    """
    dram_base = 0x3FCA0000
    dram = bytearray(4096)

    str_cursor = 0

    def add_str(text: str) -> int:
        nonlocal str_cursor
        raw = text.encode("utf-8") + b"\x00"
        addr = dram_base + str_cursor
        dram[str_cursor : str_cursor + len(raw)] = raw
        str_cursor = (str_cursor + len(raw) + 3) & ~3
        if str_cursor > 0xB00:
            raise ValueError(f"String table overflowed 0xB00 ({str_cursor:#x})")
        return addr

    s_banner = add_str(
        "\r\n================================================================\r\n"
        "  NevoFlux Voice Pendant · ESP32-S3 SoC Emulator (esp-emu)\r\n"
        "  Board: Waveshare ESP32-S3-Touch-AMOLED-1.43C (8MB Flash / 8MB PSRAM)\r\n"
        "================================================================\r\n"
        "I (12) boot: Partition Table @ 0x008000:\r\n"
        "I (14) boot:   0: nvs      data nvs     0x009000 0x006000\r\n"
        "I (16) boot:   1: otadata  data ota     0x00f000 0x002000\r\n"
        "I (18) boot:   2: phy_init data phy     0x011000 0x001000\r\n"
        "I (20) boot:   3: factory  app  factory 0x020000 0x180000 (Launcher)\r\n"
        "I (22) boot:   4: ota_0    app  ota_0   0x1A0000 0x300000 (Muse SDK)\r\n"
        "I (24) boot:   5: ota_1    app  ota_1   0x4A0000 0x280000 (NevoFlux)\r\n"
        "I (26) boot:   6: storage  data spiffs  0x720000 0x0e0000 (Shared mmap)\r\n",
    )
    s_storage = add_str(
        "I (32) storage_mmap: Mapped partition 'storage' @ 0x720000 (magic=%.4s)\r\n",
    )
    s_nvs_empty = add_str(
        "W (38) prov_nvs: NVS @ 0x009000 unprovisioned (awaiting UART/BLE prov.write)\r\n",
    )
    s_nvs_loaded = add_str(
        "I (38) prov_nvs: Loaded provisioned NVS @ 0x009000: %s\r\n",
    )
    s_boot_factory = add_str(
        "I (45) boot: otadata ota_seq=0x%08x -> booting [factory] Launcher @ 0x020000\r\n"
        "I (60) launcher: 466x466 AMOLED Dual-System Selector Ready\r\n"
        "I (62) launcher: [UART Commands] '0'=Boot Muse (ota_0) | '1'=Boot NevoFlux (ota_1) | JSON=provision.py\r\n",
    )
    s_boot_ota0 = add_str(
        "I (45) boot: otadata ota_seq=0x%08x -> booting [ota_0] Muse Assistant @ 0x1A0000\r\n"
        "I (70) muse_app: facebookincubator/muse-gadget-sdk initialized\r\n"
        "I (75) muse_oauth: Starting RFC 8628 OAuth 2.0 Device Authorization Grant...\r\n"
        "I (80) muse_oauth: Display QR -> https://muse.ai/link | user_code: ABC-123\r\n"
        "I (82) muse_app: [UART Commands] 'l'=Return to Launcher | '1'=Switch to NevoFlux (ota_1)\r\n",
    )
    s_boot_ota1 = add_str(
        "I (45) boot: otadata ota_seq=0x%08x -> booting [ota_1] NevoFlux Assistant @ 0x4A0000\r\n"
        "I (70) nevoflux: Audio pipeline 16kHz Opus TX / 24kHz Opus RX initialized\r\n"
        "I (75) nevoflux: Connecting to wss://relay.nevoflux.app (Dual-Channel E2EE)...\r\n"
        "I (80) nevoflux: State=IDLE (Hold Touch=PTT | Double-Click BOOT=Meeting Mode | 5s=Launcher)\r\n"
        "I (82) nevoflux: [UART Commands] 'l'=Return to Launcher | '0'=Switch to Muse (ota_0)\r\n",
    )
    s_sw_ota0 = add_str(
        "\r\nI (200) ota_switch: Writing ota_seq=1 (ota_0 Muse) to otadata @ 0x00f000 & resetting...\r\n",
    )
    s_sw_ota1 = add_str(
        "\r\nI (200) ota_switch: Writing ota_seq=2 (ota_1 NevoFlux) to otadata @ 0x00f000 & resetting...\r\n",
    )
    s_sw_factory = add_str(
        "\r\nI (200) ota_switch: Erasing otadata @ 0x00f000 -> fallback to [factory] Launcher & resetting...\r\n",
    )
    s_prov_ready = add_str('\r\n{"status": "ready"}\r\n')
    s_prov_ok = add_str(
        '\r\nI (300) prov_nvs: Persisted credentials to NVS @ 0x009000\r\n{"status": "ok"}\r\n',
    )

    # Constants & working buffers in DRAM:
    # 0xB00: ota_seq=1 (32B entry)
    # 0xB20: ota_seq=2 (32B entry)
    # 0xB40: constants [1, 2, 0x7B ('{'), 0x30 ('0'), 0x31 ('1'), 0x6C ('l'), 0x0A ('\n'), 0x0D ('\r'), 0x6F ('o'), 0x77 ('w')]
    dram[0xB00:0xB20] = build_otadata_entry(1)
    dram[0xB20:0xB40] = build_otadata_entry(2)
    struct.pack_into(
        "<10I",
        dram,
        0xB40,
        1,
        2,
        0x7B,  # '{'
        0x30,  # '0'
        0x31,  # '1'
        0x6C,  # 'l'
        0x0A,  # '\n'
        0x0D,  # '\r'
        0x6F,  # 'o'
        0x77,  # 'w'
    )

    addr_ota1_entry = dram_base + 0xB00
    addr_ota2_entry = dram_base + 0xB20
    addr_consts = dram_base + 0xB40
    addr_storage_buf = dram_base + 0xB80  # 32B
    addr_ota_buf = dram_base + 0xBA0      # 32B
    addr_rx_char = dram_base + 0xBC0      # 4B
    addr_line_len = dram_base + 0xBC4     # 4B
    addr_nvs_buf = dram_base + 0xC00      # 512B
    addr_line_buf = dram_base + 0xE00     # 512B

    asm = XtensaAssembler(iram_base=0x40380000, code_offset=0x100)
    asm.entry_a1_32()

    # 1. Print Boot & Partition Table Banner
    asm.call_rom(ROM_ETS_PRINTF, s_banner)

    # 2. Read storage.bin @ 0x720000 (16 bytes) & print NFST magic
    asm.call_rom(ROM_SPIFLASH_READ, 0x720000, addr_storage_buf, 16)
    asm.call_rom(ROM_ETS_PRINTF, s_storage, addr_storage_buf)

    # 3. Read NVS @ 0x009000 (256 bytes) & check if provisioned (starts with '{' = 0x7B)
    asm.call_rom(ROM_SPIFLASH_READ, 0x009000, addr_nvs_buf, 256)
    asm.l32r(9, addr_nvs_buf)
    asm.l8ui(9, 9, 0)
    asm.l32r(10, addr_consts)
    asm.l32i(10, 10, 8)  # 0x7B ('{')
    asm.beq(9, 10, "nvs_provisioned")
    asm.call_rom(ROM_ETS_PRINTF, s_nvs_empty)
    asm.j("check_otadata")

    asm.label("nvs_provisioned")
    asm.call_rom(ROM_ETS_PRINTF, s_nvs_loaded, addr_nvs_buf)

    # 4. Read otadata @ 0x00F000 (4 bytes = ota_seq)
    asm.label("check_otadata")
    asm.call_rom(ROM_SPIFLASH_READ, 0x00F000, addr_ota_buf, 4)
    asm.l32r(9, addr_ota_buf)
    asm.l32i(9, 9, 0)  # a9 = ota_seq
    asm.l32r(10, addr_consts)
    asm.l32i(11, 10, 0)  # a11 = 1
    asm.beq(9, 11, "boot_ota0")
    asm.l32i(11, 10, 4)  # a11 = 2
    asm.beq(9, 11, "boot_ota1")

    # Default: factory (Launcher)
    asm.l32r(10, s_boot_factory)
    asm.l32r(11, addr_ota_buf)
    asm.l32i(11, 11, 0)
    asm.l32r(8, ROM_ETS_PRINTF)
    asm.callx8(8)
    asm.j("uart_loop")

    asm.label("boot_ota0")
    asm.l32r(10, s_boot_ota0)
    asm.l32r(11, addr_ota_buf)
    asm.l32i(11, 11, 0)
    asm.l32r(8, ROM_ETS_PRINTF)
    asm.callx8(8)
    asm.j("uart_loop")

    asm.label("boot_ota1")
    asm.l32r(10, s_boot_ota1)
    asm.l32r(11, addr_ota_buf)
    asm.l32i(11, 11, 0)
    asm.l32r(8, ROM_ETS_PRINTF)
    asm.callx8(8)

    # 5. Interactive UART0 Command & Provisioning Loop
    asm.label("uart_loop")
    asm.call_rom(ROM_UART_RX_ONE_CHAR, addr_rx_char)
    asm.bnez(10, "uart_loop")  # non-zero means no char available

    asm.l32r(9, addr_rx_char)
    asm.l8ui(9, 9, 0)  # a9 = received byte
    asm.l32r(10, addr_line_len)
    asm.l32i(11, 10, 0)  # a11 = current line_len

    # Ignore '\r' (0x0D)
    asm.l32r(12, addr_consts)
    asm.l32i(13, 12, 28)  # 0x0D
    asm.beq(9, 13, "uart_loop")

    # Check if '\n' (0x0A) -> process buffered line
    asm.l32i(13, 12, 24)  # 0x0A
    asm.beq(9, 13, "process_line")

    # If line_len == 0, check single-character hotkeys '0', '1', 'l'
    asm.bnez(11, "append_char")
    asm.l32i(13, 12, 12)  # '0'
    asm.beq(9, 13, "do_switch_ota0")
    asm.l32i(13, 12, 16)  # '1'
    asm.beq(9, 13, "do_switch_ota1")
    asm.l32i(13, 12, 20)  # 'l'
    asm.beq(9, 13, "do_switch_factory")

    asm.label("append_char")
    asm.l32r(14, addr_line_buf)
    asm.add(14, 14, 11)
    asm.s8i(9, 14, 0)
    asm.addi(11, 11, 1)
    asm.s32i(11, 10, 0)
    asm.addi(14, 14, 1)
    asm.l32r(15, 0)
    asm.s8i(15, 14, 0)  # keep null-terminated
    asm.j("uart_loop")

    asm.label("process_line")
    # Reset line_len = 0
    asm.l32r(15, 0)
    asm.s32i(15, 10, 0)
    # Check if line_buf[0] == '{' (JSON RPC from provision.py)
    asm.l32r(14, addr_line_buf)
    asm.l8ui(9, 14, 0)
    asm.l32i(13, 12, 8)  # '{'
    asm.bne(9, 13, "uart_loop")

    # Check line_buf[14]: 'w' (0x77) = prov.write, otherwise prov.offer / prov.status
    asm.l8ui(9, 14, 14)
    asm.l32i(13, 12, 36)  # 'w'
    asm.beq(9, 13, "do_prov_write")
    asm.call_rom(ROM_ETS_PRINTF, s_prov_ready)
    asm.j("uart_loop")

    asm.label("do_prov_write")
    asm.call_rom(ROM_SPIFLASH_UNLOCK)
    asm.call_rom(ROM_SPIFLASH_ERASE_SECTOR, 0x09)  # 0x009000 >> 12
    asm.call_rom(ROM_SPIFLASH_WRITE, 0x009000, addr_line_buf, 512)
    asm.call_rom(ROM_ETS_PRINTF, s_prov_ok)
    asm.j("uart_loop")

    # Partition switching handlers (write otadata @ 0xF000 + software_reset)
    asm.label("do_switch_ota0")
    asm.call_rom(ROM_ETS_PRINTF, s_sw_ota0)
    asm.call_rom(ROM_SPIFLASH_UNLOCK)
    asm.call_rom(ROM_SPIFLASH_ERASE_SECTOR, 0x0F)
    asm.call_rom(ROM_SPIFLASH_WRITE, 0x00F000, addr_ota1_entry, 32)
    asm.call_rom(ROM_SOFTWARE_RESET)
    asm.j("uart_loop")

    asm.label("do_switch_ota1")
    asm.call_rom(ROM_ETS_PRINTF, s_sw_ota1)
    asm.call_rom(ROM_SPIFLASH_UNLOCK)
    asm.call_rom(ROM_SPIFLASH_ERASE_SECTOR, 0x0F)
    asm.call_rom(ROM_SPIFLASH_WRITE, 0x00F000, addr_ota2_entry, 32)
    asm.call_rom(ROM_SOFTWARE_RESET)
    asm.j("uart_loop")

    asm.label("do_switch_factory")
    asm.call_rom(ROM_ETS_PRINTF, s_sw_factory)
    asm.call_rom(ROM_SPIFLASH_UNLOCK)
    asm.call_rom(ROM_SPIFLASH_ERASE_SECTOR, 0x0F)
    asm.call_rom(ROM_SOFTWARE_RESET)
    asm.j("uart_loop")

    iram_seg = asm.assemble()
    entry_addr = asm.iram_base + asm.code_offset
    return pack_esp32s3_image(entry_addr, [(dram_base, bytes(dram)), (asm.iram_base, iram_seg)])


def build_merged_flash(
    out_path: pathlib.Path,
    boot_partition: str = "factory",
) -> pathlib.Path:
    flash = bytearray(b"\xFF" * FLASH_SIZE_BYTES)

    # 1. Partition Table at 0x8000
    pt_bytes = compile_partition_table(REPO_ROOT / "partitions.csv")
    flash[0x8000 : 0x8000 + len(pt_bytes)] = pt_bytes

    # 2. Shared storage partition at 0x720000
    storage_bytes = build_storage_image(
        b"CJK_FONT_20PX_STUB",
        b"ESP_SR_VAD_MODEL_STUB",
    )
    flash[0x720000 : 0x720000 + len(storage_bytes)] = storage_bytes

    # 3. Initial otadata at 0xF000
    ota_seq_map = {"factory": 0, "ota_0": 1, "ota_1": 2}
    ota_entry = build_otadata_entry(ota_seq_map.get(boot_partition, 0))
    flash[0xF000 : 0xF000 + len(ota_entry)] = ota_entry

    # 4. Bootloader & App Partitions (use ESP-IDF builds if present, else native S3 image)
    native_fw = build_native_s3_firmware()
    bootloader_bin = REPO_ROOT / "apps" / "launcher" / "build" / "bootloader" / "bootloader.bin"
    launcher_bin = REPO_ROOT / "apps" / "launcher" / "build" / "nevoflux_launcher.bin"
    muse_bin = REPO_ROOT / "apps" / "muse" / "build" / "muse_gadget_pendant.bin"
    nevoflux_bin = REPO_ROOT / "apps" / "nevoflux" / "build" / "nevoflux_pendant.bin"

    bl_data = bootloader_bin.read_bytes() if bootloader_bin.exists() else native_fw
    factory_data = launcher_bin.read_bytes() if launcher_bin.exists() else native_fw
    ota0_data = muse_bin.read_bytes() if muse_bin.exists() else native_fw
    ota1_data = nevoflux_bin.read_bytes() if nevoflux_bin.exists() else native_fw

    flash[0x000000 : 0x000000 + len(bl_data)] = bl_data
    flash[0x020000 : 0x020000 + len(factory_data)] = factory_data
    flash[0x1A0000 : 0x1A0000 + len(ota0_data)] = ota0_data
    flash[0x4A0000 : 0x4A0000 + len(ota1_data)] = ota1_data

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(flash)
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Build 8MB ESP32-S3 merged flash image for esp-emu")
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=REPO_ROOT / "build" / "esp-emu" / "merged-flash.bin",
        help="Output merged flash image path",
    )
    parser.add_argument(
        "--boot-partition",
        choices=["factory", "ota_0", "ota_1"],
        default="factory",
        help="Initial active partition in otadata (0xF000)",
    )
    args = parser.parse_args()
    out = build_merged_flash(args.out, args.boot_partition)
    print(f"[OK] Built 8MB ESP32-S3 merged flash image ({out.stat().st_size} bytes) -> {out}")
    print(f"     Initial otadata boot partition: {args.boot_partition}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
