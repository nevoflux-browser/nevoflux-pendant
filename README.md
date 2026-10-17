# nevoflux-pendant

ESP32-S3 firmware monorepo for the **NevoFlux Voice Pendant** — dual-boot **Muse** (`ota_0`) / **NevoFlux** (`ota_1`) via a unified **Launcher** (`factory`), providing a wearable voice interface where all STT, LLM inference, and TTS run locally on your own NevoFlux daemon.

## Hardware Target

**Waveshare ESP32-S3-Touch-AMOLED-1.43C**

| Component | Specification |
|---|---|
| SoC | ESP32-S3-PICO-1-N8R8 (8MB QIO Flash / 8MB Octal PSRAM, 512KB Internal SRAM) |
| Display | 1.43" Round AMOLED (466×466), SH8601 controller over QSPI |
| Touch | FT3168 / CST816 capacitive touch over I2C |
| Audio | ES8311 (Mono DAC + PA) + ES7210 (Dual-Mic ADC) sharing `I2S_NUM_0` clocks |
| Power | Direct battery voltage divider via `ADC1_CH3` (no PMU) |
| Buttons | `BOOT` (`GPIO0`: Hold = Push-to-Talk, Double-Click = Meeting Mode, Hold 2s on boot = Launcher Escape), `PWR` (`GPIO1`: Cancel / Return) |

## Repository Layout

```text
nevoflux-pendant/
├── partitions.csv                 # 8MB Flash partition layout (factory 1MB, ota_0 3MB, ota_1 2.5MB, storage 1.375MB)
├── Makefile                       # Host-side gcc & python3 test runner and storage partition packer
├── components/
│   ├── nf_proto/                  # NFA1 (24B BE) & NFM1 (22+idLen BE) codecs, WAV parser, SequenceTracker, AES-256-GCM
│   ├── prov_nvs/                  # Shared NVS (pendant / nf_cfg) + USB Serial JSON-RPC with 15s BOOT confirm
│   └── bsp_143c/                  # Waveshare 1.43C pure-C BSP (SH8601 QSPI, ES8311/ES7210 shared I2S0, BOOT gestures)
├── apps/
│   ├── launcher/                  # factory partition: Boot selector UI + BLE GATT / USB Serial provisioning
│   └── nevoflux/                  # ota_1 partition: NevoFlux private voice client (BLE disabled, dual-core PSRAM architecture)
└── tools/
    ├── provision/                 # Python 3 CLI: Device Grant + Argon2id(m=65536,t=3,p=4) KDF + USB Serial injection
    ├── pack_storage/              # Pack shared storage partition image (20px CJK bitmap font + ESP-SR VAD model)
    └── sim/                       # 1.43" (466×466) round AMOLED UI simulator (SDL2 desktop + Web UI)
```

## 8MB Flash Partition Table (`partitions.csv`)

| Name | Type / SubType | Offset | Size | Description |
|---|---|---|---|---|
| `nvs` | `data` / `nvs` | `0x009000` | 24 KB | Shared `pendant` WiFi config & `nf_cfg` pairing credentials |
| `otadata` | `data` / `ota` | `0x00F000` | 8 KB | Bootloader active OTA selector (erasing falls back to `factory`) |
| `phy_init` | `data` / `phy` | `0x011000` | 4 KB | RF calibration data |
| `factory` | `app` / `factory` | `0x020000` | **1.0 MB** | **Launcher**: Dual-boot UI + NimBLE GATT & USB Serial provisioning |
| `ota_0` | `app` / `ota_0` | `0x120000` | **3.0 MB** | **Muse Partition**: Community port fork (`wupsbr`) |
| `ota_1` | `app` / `ota_1` | `0x420000` | **2.5 MB** | **NevoFlux Partition**: Native C client (`CONFIG_BT_ENABLED=n`) |
| `storage` | `data` / `spiffs` | `0x6A0000` | **1.375 MB** | Shared zero-copy `mmap` image (20px CJK font + ESP-SR VAD model) |

## Quick Start

### 1. Run Host Unit & Integration Tests (`gcc` + `python3`)

All wire codecs (`NFA1`, `NFM1`, WAV header parser, `SequenceTracker`), AES-256-GCM framing, NVS provisioning handshakes, I2S sample-rate switching, and dual-mode PTT/Meeting state machines run natively on Linux without hardware:

```bash
make test
```

### 2. Launch the 1.43" (466×466) Circular AMOLED UI Simulator

You can preview and interact with the Launcher (`factory`), Push-to-Talk (`PTT`), TTS Playback (`24kHz` / `48kHz`), and Meeting Mode UIs on Linux via either the Web Simulator or the native SDL2 desktop app:

```bash
# Start interactive Web Simulator on http://localhost:8765
make sim-web

# Or run the native SDL2 desktop window
make -C tools/sim run-sdl
```

### 3. Provision WiFi & NevoFlux Pairing Credentials

Run `/pair-device` on your NevoFlux desktop daemon to get a pairing code, then connect the pendant via USB:

```bash
python3 tools/provision/provision.py \
  --port /dev/ttyACM0 \
  --ssid "<WIFI_SSID>" \
  --password "<WIFI_PASS>" \
  --code "<PAIRING_CODE>"
```

- The tool performs an independent **Device Authorization Grant** (minting a dedicated 7-day `account_token` for the pendant so it can be revoked independently if lost) and derives the 32-byte AES-256-GCM keys on the host using `Argon2id (m=65536, t=3, p=4)`.
- When prompted on the pendant screen, **click the `BOOT` button once within 15 seconds** to confirm physical presence and commit the credentials to NVS.

### 4. Build & Flash ESP32-S3 Firmware (ESP-IDF)

```bash
# 1. Pack shared storage image (CJK font + VAD model)
make storage

# 2. Build Launcher (factory @ 0x20000)
idf.py -C apps/launcher set-target esp32s3 && idf.py -C apps/launcher build

# 3. Build NevoFlux Firmware (ota_1 @ 0x420000)
idf.py -C apps/nevoflux set-target esp32s3 && idf.py -C apps/nevoflux build

# 4. Flash partitions via esptool.py
esptool.py --chip esp32s3 write_flash \
  0x20000  apps/launcher/build/nevoflux_launcher.bin \
  0x420000 apps/nevoflux/build/nevoflux_pendant.bin \
  0x6A0000 build/storage.bin
```

## License

Licensed under the [Apache License, Version 2.0](file:///ai/project/nevoflux-pendant/LICENSE).

