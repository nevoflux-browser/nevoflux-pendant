#!/usr/bin/env bash
# ==============================================================================
# NevoFlux Voice Pendant · ESP32-S3 SoC Emulator Runner (espressif/esp-emulator)
#
# Usage:
#   ./tools/emu/run_esp_emu.sh                   # Interactive ESP32-S3 session
#   ./tools/emu/run_esp_emu.sh --demo-ota        # Auto-verify factory -> ota_0 -> ota_1 -> factory
#   ./tools/emu/run_esp_emu.sh --demo-provision  # Auto-verify provision.py over --uart-tcp + NVS persistence
#   ./tools/emu/run_esp_emu.sh --boot-partition ota_0
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FLASH_IMG="${REPO_ROOT}/build/esp-emu/merged-flash.bin"

# Locate or install esp-emu
if [[ -x "${SCRIPT_DIR}/bin/esp-emu" ]]; then
    ESP_EMU="${SCRIPT_DIR}/bin/esp-emu"
elif [[ -x "${HOME}/.local/bin/esp-emu" ]]; then
    ESP_EMU="${HOME}/.local/bin/esp-emu"
elif command -v esp-emu >/dev/null 2>&1; then
    ESP_EMU="$(command -v esp-emu)"
else
    echo "[INFO] esp-emu not found; installing to ${SCRIPT_DIR}/bin/esp-emu..."
    mkdir -p "${SCRIPT_DIR}/bin"
    curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh \
        | sh -s -- --bin-dir "${SCRIPT_DIR}/bin"
    ESP_EMU="${SCRIPT_DIR}/bin/esp-emu"
fi

MODE="interactive"
BOOT_PARTITION="factory"
REBUILD=1
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --demo-ota)
            MODE="demo-ota"
            shift
            ;;
        --demo-provision)
            MODE="demo-provision"
            shift
            ;;
        --boot-partition)
            BOOT_PARTITION="${2:?--boot-partition requires factory|ota_0|ota_1}"
            shift 2
            ;;
        --no-rebuild)
            REBUILD=0
            shift
            ;;
        -h|--help)
            sed -n '2,10p' "$0"
            exit 0
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

if [[ "${REBUILD}" -eq 1 ]] || [[ ! -f "${FLASH_IMG}" ]]; then
    python3 "${SCRIPT_DIR}/build_flash_image.py" \
        --out "${FLASH_IMG}" \
        --boot-partition "${BOOT_PARTITION}"
fi

COMMON_FLAGS=(
    --chip esp32s3
    --psram-size 8M
    --net user
    --wifi-ssid "${WIFI_SSID:-NevoFlux-WiFi}"
    --wifi-password "${WIFI_PASS:-nevoflux123}"
    --firmware "${FLASH_IMG}"
)

case "${MODE}" in
    demo-ota)
        echo "[DEMO] Running automated Dual-OTA partition switching test on ESP32-S3..."
        exec "${ESP_EMU}" "${COMMON_FLAGS[@]}" \
            --inject-on "Dual-System Selector Ready" \
            --inject "0" \
            --inject-on "user_code: ABC-123" \
            --inject "1" \
            --inject-on "Double-Click BOOT=Meeting Mode" \
            --inject "l" \
            --exit-on "fallback to [factory] Launcher" \
            --timeout 5s \
            "${EXTRA_ARGS[@]}"
        ;;

    demo-provision)
        UART_PORT="${UART_PORT:-15555}"
        echo "[DEMO] 1/2 Starting esp-emu with --uart-tcp 127.0.0.1:${UART_PORT} and --save-state..."
        "${ESP_EMU}" "${COMMON_FLAGS[@]}" \
            --uart-tcp "127.0.0.1:${UART_PORT}" \
            --save-state \
            --exit-on '{"status": "ok"}' \
            --timeout 15s &
        EMU_PID=$!
        trap 'kill "${EMU_PID}" 2>/dev/null || true' EXIT
        sleep 1

        echo "[DEMO] 2/2 Running tools/provision/provision.py against tcp://127.0.0.1:${UART_PORT}..."
        python3 "${REPO_ROOT}/tools/provision/provision.py" \
            --code "NEVO-8628" \
            --token "tok_emu_independent_7d" \
            --ssid "NevoFlux-WiFi" \
            --password "nevoflux123" \
            --port "tcp://127.0.0.1:${UART_PORT}"

        wait "${EMU_PID}" || true
        trap - EXIT

        echo "[DEMO] Rebooting esp-emu from saved flash image to verify NVS persistence @ 0x009000..."
        exec "${ESP_EMU}" "${COMMON_FLAGS[@]}" \
            --exit-on "Loaded provisioned NVS @ 0x009000" \
            --timeout 3s
        ;;

    interactive)
        exec "${ESP_EMU}" "${COMMON_FLAGS[@]}" "${EXTRA_ARGS[@]}"
        ;;
esac
