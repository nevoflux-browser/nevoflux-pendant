# nevoflux-pendant

ESP32-S3 firmware monorepo for the **NevoFlux Voice Pendant** — dual-boot **Muse** (`ota_0`) / **NevoFlux** (`ota_1`) via a unified **Launcher** (`factory`), providing a wearable voice interface where all STT, LLM inference, and TTS run locally on your own NevoFlux daemon.

## Hardware Target

**Waveshare ESP32-S3-Touch-AMOLED-1.43C**

| Component | Specification |
|---|---|
| SoC | ESP32-S3-PICO-1-N8R8 (8MB QIO Flash / 8MB Octal PSRAM, 512KB Internal SRAM) |
| Display | 1.43" Round AMOLED (466×466), CO5300 controller over QSPI (SPI2 @ 40 MHz; driven via `esp_lcd_sh8601`) |
| Touch | CST820 capacitive touch over I2C (`0x15`, shared bus GPIO47/48) |
| Audio | ES8311 (Mono DAC + PA) + ES7210 (Dual-Mic ADC) sharing `I2S_NUM_0` clocks |
| Power | Direct battery voltage divider via `ADC1_CH3` (no PMU) |
| Buttons | `BOOT` (`GPIO0`: Hold = Push-to-Talk, Double-Click = Meeting Mode, Hold 2s on boot = Launcher Escape), `PWR` (wired to the power-latch IC, not readable by firmware) |

Pin assignments in `components/bsp_143c/include/bsp_143c_pins.h` follow the official [Waveshare demo `bsp_config.h`](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C) and schematic.

## Repository Layout

```text
nevoflux-pendant/
├── partitions.csv                 # 8MB Flash partition layout (factory 1MB, ota_0 3MB, ota_1 2.5MB, storage 1.375MB)
├── Makefile                       # Host-side gcc & python3 test runner and storage partition packer
├── components/
│   ├── nf_proto/                  # NFA1 (24B BE) & NFM1 (22+idLen BE) codecs, WAV parser, SequenceTracker, AES-256-GCM
│   ├── prov_nvs/                  # Shared NVS (pendant / muse_cfg / nf_cfg) + USB Serial JSON-RPC with 15s BOOT confirm
│   └── bsp_143c/                  # Waveshare 1.43C BSP (CO5300 QSPI + LVGL 9 UI, CST820 touch, ES8311/ES7210 I2S0, BOOT gestures)
│   └── pendant_wifi/              # Shared WiFi STA (credentials from `pendant` NVS, auto-reconnect)
├── apps/
│   ├── launcher/                  # factory partition: Boot selector UI + BLE GATT / USB Serial provisioning
│   ├── muse/                      # ota_0 partition: Official facebookincubator/muse-gadget-sdk + OAuth 2.0 Device Flow (RFC 8628)
│   └── nevoflux/                  # ota_1 partition: NevoFlux private voice client (BLE disabled, dual-core PSRAM architecture)
└── tools/
    ├── provision/                 # Python 3 CLI: Device Grant + Argon2id(m=65536,t=3,p=4) KDF + USB Serial / TCP injection
    ├── pack_storage/              # Pack shared storage partition image (20px CJK bitmap font + ESP-SR VAD model)
    ├── sim/                       # 1.43" (466×466) round AMOLED UI simulator (SDL2 desktop + Web UI)
    └── emu/                       # ESP32-S3 SoC & Flash emulator runner (espressif/esp-emulator)
```

## 8MB Flash Partition Table (`partitions.csv`)

| Name | Type / SubType | Offset | Size | Description |
|---|---|---|---|---|
| `nvs` | `data` / `nvs` | `0x009000` | 24 KB | Shared `pendant` WiFi config, `muse_cfg` OAuth token, and `nf_cfg` pairing credentials |
| `otadata` | `data` / `ota` | `0x00F000` | 8 KB | Bootloader active OTA selector (erasing falls back to `factory`) |
| `phy_init` | `data` / `phy` | `0x011000` | 4 KB | RF calibration data |
| `factory` | `app` / `factory` | `0x020000` | **1.0 MB** | **Launcher**: Dual-boot UI + NimBLE GATT & USB Serial provisioning |
| `ota_0` | `app` / `ota_0` | `0x120000` | **3.0 MB** | **Muse Partition**: Official [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk) + RFC 8628 Device Flow |
| `ota_1` | `app` / `ota_1` | `0x420000` | **2.5 MB** | **NevoFlux Partition**: Native C client (`CONFIG_BT_ENABLED=n`) |
| `storage` | `data` / `spiffs` | `0x6A0000` | **1.375 MB** | Shared zero-copy `mmap` image (20px CJK font + ESP-SR VAD model) |

## Dual-System Authentication Flows

### Muse Partition (`ota_0`): On-Device OAuth 2.0 Device Authorization Grant (RFC 8628)
Built on the official [Meta Muse Gadget ESP32 SDK](https://github.com/facebookincubator/muse-gadget-sdk) (`apps/muse`), leveraging the 1.43" (466×466) AMOLED display for zero-token factory flashing:
1. **Zero Pre-baked Token**: Factory firmware requires no hardcoded user SDK token.
2. **Device Code Request**: After joining Wi-Fi (from shared `pendant` NVS), the ESP32 requests `device_code` and `user_code` from the Muse authentication endpoint.
3. **On-Screen QR & Short Code**: The 466×466 round screen renders a login QR code and short code prompt (e.g., *"Visit `muse.ai/link` and enter code `ABC-123`"*).
4. **Browser Approval & Auto-NVS Persistence**: While the user approves in their phone/PC browser, the ESP32 polls in the background (honoring RFC 8628 `authorization_pending` and `slow_down` intervals). Once approved, the issued long-lived token is automatically saved into NVS (`muse_cfg`).

### NevoFlux Partition (`ota_1`): End-to-End Encrypted Private Daemon Pairing
Uses `tools/provision/provision.py` to mint a dedicated 7-day device `account_token` and derive 32-byte AES-256-GCM channel keys (`Argon2id m=65536, t=3, p=4`), requiring a physical `BOOT` button press within 15 seconds on the pendant before committing to NVS (`nf_cfg`).

## Quick Start

### 1. Run Host Unit & Integration Tests (`gcc` + `python3`)

All wire codecs (`NFA1`, `NFM1`, WAV header parser, `SequenceTracker`), AES-256-GCM framing, NVS provisioning handshakes, Muse RFC 8628 Device Flow state machines, I2S sample-rate switching, and dual-mode PTT/Meeting state machines run natively on Linux without hardware:

```bash
make test
```

### 2. Launch the 1.43" (466×466) Circular AMOLED UI Simulator

You can preview and interact with the Launcher (`factory`), Muse RFC 8628 QR/Short-Code Pairing (`ota_0`), Push-to-Talk (`PTT`), TTS Playback (`24kHz` / `48kHz`), and Meeting Mode UIs on Linux via either the Web Simulator or the native SDL2 desktop app:

```bash
# Start interactive Web Simulator on http://localhost:8765
make sim-web

# Or run the native SDL2 desktop window
make -C tools/sim run-sdl
```

### 3. Run the ESP32-S3 SoC & Flash Emulator (`espressif/esp-emulator`)

Using Espressif's [`esp-emulator` (`esp-emu`)](https://github.com/espressif/esp-emulator), you can boot a full 8MB merged ESP32-S3 flash image (`partitions.csv` + `storage.bin` + `otadata` + `factory`/`ota_0`/`ota_1`) directly from the real ESP32-S3 Mask ROM (`ESP-ROM:esp32s3-20210327`) with 8MB Octal PSRAM and simulated 2.4GHz Wi-Fi:

```bash
# Automated Dual-OTA partition switching test (factory -> ota_0 -> ota_1 -> factory)
make emu-ota

# Automated UART TCP provisioning (provision.py -> NVS @ 0x009000 persistence check)
make emu-provision

# Interactive ESP32-S3 session (type '0' for Muse, '1' for NevoFlux, 'l' for Launcher)
make emu
```

### 4. Provision WiFi & NevoFlux Pairing Credentials

Run `/pair-device` on your NevoFlux desktop daemon to get a pairing code, then connect the pendant via USB (or `tcp://127.0.0.1:15555` when running `esp-emu`):

```bash
python3 tools/provision/provision.py \
  --port /dev/ttyACM0 \
  --ssid "<WIFI_SSID>" \
  --password "<WIFI_PASS>" \
  --code "<PAIRING_CODE>"
```

- The tool performs an independent **Device Authorization Grant** (minting a dedicated 7-day `account_token` for the pendant so it can be revoked independently if lost) and derives the 32-byte AES-256-GCM keys on the host using `Argon2id (m=65536, t=3, p=4)`.
- When prompted on the pendant screen, **click the `BOOT` button once within 15 seconds** to confirm physical presence and commit the credentials to NVS.

### 5. Build & Flash ESP32-S3 Firmware (ESP-IDF)

Builds all three apps with ESP-IDF v5.3.2 in Docker (no local IDF needed) and writes flashable images to `build/firmware/`:

```bash
make firmware
# optional: bake a fallback WiFi used only while NVS has none
WIFI_SSID="MyRouter" WIFI_PASS="secret" make firmware
```

Flash the merged 8MB image (first flash; this also erases NVS):

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x0 build/firmware/nevoflux-pendant-8MB.bin
```

Or flash the individual images and keep NVS (WiFi / pairing) intact:

```bash
cd build/firmware && esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args
```

If the board does not enumerate, hold `BOOT`, tap `RESET`, release `BOOT` to enter download mode.

Write WiFi only (no NevoFlux pairing needed); click `BOOT` on the pendant within 15s when prompted:

```bash
python3 tools/provision/provision.py --wifi-only --port /dev/ttyACM0 --ssid "MyRouter" --password "secret"
```

The Launcher then shows `SSID · IP · RSSI` under the title once the router assigns an address.

UI fonts (`components/bsp_143c/src/fonts/`) are subset to the strings used in the firmware; regenerate with `python3 tools/fonts/gen_ui_font.py` after adding new Chinese UI text.

## License

Licensed under the [Apache License, Version 2.0](file:///ai/project/nevoflux-pendant/LICENSE).

