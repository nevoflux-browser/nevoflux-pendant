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
├── partitions.csv                 # 8MB Flash partition layout (factory 1.5MB, ota_0 3MB, ota_1 2.5MB, storage 896KB)
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
| `factory` | `app` / `factory` | `0x020000` | **1.5 MB** | **Launcher**: Dual-boot UI + NimBLE (Nordic UART Service) & USB Serial provisioning |
| `ota_0` | `app` / `ota_0` | `0x1A0000` | **3.0 MB** | **Muse Partition**: Official [`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk) + RFC 8628 Device Flow |
| `ota_1` | `app` / `ota_1` | `0x4A0000` | **2.5 MB** | **NevoFlux Partition**: Native C client (`CONFIG_BT_ENABLED=n`) |
| `storage` | `data` / `spiffs` | `0x720000` | **896 KB** | Shared zero-copy `mmap` image (20px CJK font + ESP-SR VAD model) |

## Dual-System Authentication Flows

### Muse Partition (`ota_0`): On-Device OAuth 2.0 Device Authorization Grant (RFC 8628)
Built on the official [Meta Muse Gadget ESP32 SDK](https://github.com/facebookincubator/muse-gadget-sdk) (`apps/muse`), leveraging the 1.43" (466×466) AMOLED display for zero-token factory flashing:
1. **Zero Pre-baked Token**: Factory firmware requires no hardcoded user SDK token.
2. **Device Code Request**: After joining Wi-Fi (from shared `pendant` NVS), the ESP32 requests `device_code` and `user_code` from the Muse authentication endpoint.
3. **On-Screen QR & Short Code**: The 466×466 round screen renders a login QR code and short code prompt (e.g., *"Visit `muse.ai/link` and enter code `ABC-123`"*).
4. **Browser Approval & Auto-NVS Persistence**: While the user approves in their phone/PC browser, the ESP32 polls in the background (honoring RFC 8628 `authorization_pending` and `slow_down` intervals). Once approved, the issued long-lived token is automatically saved into NVS (`muse_cfg`).

### NevoFlux Partition (`ota_1`): End-to-End Encrypted Private Daemon Pairing
Uses `tools/provision/provision.py` to mint a dedicated device `account_token` and derive the 32-byte AES-256-GCM channel keys (`Argon2id m=65536, t=3, p=4`) from the desktop's `/pair-device` link and code, requiring a physical `BOOT` button press within 15 seconds on the pendant before committing to NVS (`nf_cfg`).

### Talking to the desktop: LAN first, Relay otherwise
The pendant speaks the nevoflux-agent remote protocol (the same one the phone portal uses): a control channel (`sync` → `sessions` → `attach` the most recently updated conversation) and a data channel. Every frame is `{"k":"frame","frame":…}` sealed with AES-256-GCM (`nonce ‖ ciphertext ‖ tag`).

- **Same Wi-Fi as the desktop**: the agent listens on TCP `47821` and advertises mDNS `_nevoflux._tcp` (TXT `ch` = first 8 hex of SHA-256 of each data channel id). The pendant dials `ws://<pc>:47821/?c=<channel>` directly: no Cloudflare traffic, no cost, lowest latency. The status line shows **局域网**. (Windows asks once to allow the agent through the firewall; `NEVOFLUX_LAN=0` turns it off.)
- **Anywhere else**: `wss://relay.nevoflux.app/?c=<channel>&t=<15-min JWT>`; the status line shows **Relay**. While on the relay the pendant re-checks the LAN every 2 minutes.
- **Voice**: hold `BOOT` and speak. 16 kHz PCM goes up as one `speech_chunk` every 200 ms (5 relay messages/s). The desktop transcribes it (`speech_final`), and the pendant sends the turn as `user_message {voice:true}`. The answer streams back as text (`stream_delta`) and as spoken sentences (`voice_audio`, a base64 WAV, resampled to 16 kHz on the pendant so the mic never stops). Press `BOOT` while it speaks to interrupt it (`voice_barge_in`). Press `PWR` to cancel.

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

Run `/pair-device` in the NevoFlux sidebar. It prints a link (`https://…/d/<control_channel_id>/pair`) and a code (`X-XXXX-XXXX-XXXX`). Keep NevoFlux running, then connect the pendant via USB (or `tcp://127.0.0.1:15555` when running `esp-emu`):

```bash
pip install pyserial websocket-client cryptography argon2-cffi
python3 tools/provision/provision.py \
  --port /dev/ttyACM0 \
  --ssid "<WIFI_SSID>" \
  --password "<WIFI_PASS>" \
  --link "<LINK FROM /pair-device>" \
  --code "<PAIRING_CODE>"
```

- The tool runs an independent **Device Authorization Grant** for the pendant, so its session can be revoked on its own if the pendant is lost. Sign in with the **same NevoFlux account** as the desktop.
- It derives both channel keys with `Argon2id (m=65536, t=3, p=4)` on the PC, then asks the desktop for the data channel id over the sealed control channel. Pass `--data-channel` to skip that step.
- When prompted on the pendant screen, **click the `BOOT` button once within 15 seconds** to confirm physical presence and commit the credentials to NVS.

**Bluetooth LE** works the same way (`pip install bleak`; Windows 10/11, Linux, macOS). BLE is **off by default**: tap **开启蓝牙配网** on the Launcher screen to advertise `NevoFlux-XXXX` (shown on screen) with the Nordic UART Service for 3 minutes. It closes automatically after a successful write, on timeout, or when tapped again:

```bash
python3 tools/provision/provision.py --wifi-only --port ble --ssid "<WIFI_SSID>" --password "<WIFI_PASS>"
# pick a specific pendant: --port ble:NevoFlux-1A2B  (or its BLE address)
```

Any BLE UART app (e.g. nRF Connect / nRF Toolbox) can also be used: write one JSON request per line to RX `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` and read replies from TX `6E400003-…` notifications — `{"cmd":"prov.offer","device":"Phone"}`, click `BOOT`, then `{"cmd":"prov.wifi","wifi_ssid":"…","wifi_pass":"…"}`.
The BLE link is not encrypted (no pairing); opening the window on screen and the `BOOT` press are the gates, so provision in a trusted place.

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

Audio (ES8311 speaker DAC + ES7210 mics) uses `espressif/esp_codec_dev` with the same TDM setup as the official Waveshare `04_Audio_Test`. When NevoFlux starts it plays a short 1 kHz beep and logs `bsp_audio: self-test: mic ch0/ch1 rms=… peak=…`; disable with `CONFIG_BSP_AUDIO_BOOT_SELFTEST=n` (volume / mic gain: `CONFIG_BSP_AUDIO_SPK_VOLUME`, `CONFIG_BSP_AUDIO_MIC_GAIN_DB`).

### 6. Flashing on Windows 10

1. **Copy the files** to the Windows PC (e.g. `C:\fw\`): `build/firmware/nevoflux-pendant-8MB.bin` and `tools/provision/provision.py`. Optionally verify the image against `build/firmware/SHA256SUMS`:

   ```bat
   certutil -hashfile C:\fw\nevoflux-pendant-8MB.bin SHA256
   ```

2. **Install Python 3** (tick "Add python.exe to PATH"), then:

   ```bat
   pip install esptool pyserial bleak websocket-client cryptography argon2-cffi
   ```

3. **Power on and find the COM port.** Connect the board over USB-C (the ESP32-S3 native USB uses the built-in Win10 driver), then **long-press `PWR` for 2–3 s** — the board has a power latch and stays off (dark screen, "USB device not recognized" / Code 43) until powered on. In Device Manager → *Ports (COM & LPT)*, note the "USB Serial Device (COMx)".

   If no port appears (e.g. Code 43 while the factory demo runs) or flashing fails to connect, enter download mode: hold `BOOT`, long-press `PWR` (or tap `RESET` if already on), release `PWR`, then release `BOOT`. The screen stays dark in download mode and the COM number may change.

4. **Flash** (examples use `COM5`):

   ```bat
   python -m esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x0 C:\fw\nevoflux-pendant-8MB.bin
   ```

   Success ends with `Hash of data verified.` On errors retry with `-b 460800`. If you used download mode, press `RESET` afterwards (long-press `PWR` if the screen stays dark). The merged image erases NVS, so WiFi must be provisioned again after every full flash.

5. **Provision WiFi** once the Launcher is running (not in download mode), and click `BOOT` within 15s when the screen prompts. Only 2.4 GHz networks are supported.

   ```bat
   python C:\fw\provision.py --wifi-only --port COM5 --ssid "MyRouter" --password "secret"
   ```

   Or over Bluetooth (PC Bluetooth on, no cable needed; first tap **开启蓝牙配网** on the Launcher): `python C:\fw\provision.py --wifi-only --port ble --ssid "MyRouter" --password "secret"`.

6. **Serial log** for troubleshooting (close other programs using the port, press `RESET`, exit with `Ctrl+]`):

   ```bat
   python -m serial.tools.miniterm COM5 115200
   ```

**GUI alternative:** Espressif *Flash Download Tool* — ChipType `ESP32-S3`, WorkMode `Develop`, LoadMode `USB`; add only `nevoflux-pendant-8MB.bin` @ `0x0` (ticked); SPI SPEED `80MHz`, SPI MODE `DIO`, FLASH SIZE `64Mbit`; select the COM port, BAUD `921600`, START. Then provision WiFi with step 5.

## License

Licensed under the [Apache License, Version 2.0](file:///ai/project/nevoflux-pendant/LICENSE).

