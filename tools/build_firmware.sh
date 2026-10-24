#!/usr/bin/env bash
# Build real, flashable ESP32-S3 firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.43C
# using ESP-IDF in Docker (no local IDF install needed).
#
# Output (build/firmware/):
#   bootloader.bin            @ 0x0
#   partition-table.bin       @ 0x8000
#   ota_data_initial.bin      @ 0xF000   (blank otadata -> boots factory Launcher)
#   nevoflux_launcher.bin     @ 0x20000  (factory, 1.5MB)
#   muse_gadget_pendant.bin   @ 0x1A0000 (ota_0)
#   nevoflux_pendant.bin      @ 0x4A0000 (ota_1)
#   storage.bin               @ 0x720000
#   nevoflux-pendant-8MB.bin  merged image, flash at 0x0 (erases NVS -> WiFi must be re-provisioned)
#   flash_args                esptool argument file for flashing the individual images (keeps NVS)
#
# Usage:
#   tools/build_firmware.sh                     # build everything
#   WIFI_SSID=MyAP WIFI_PASS=secret tools/build_firmware.sh
#                                               # bake a fallback WiFi used when NVS has none
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.3.2}"
OUT="$REPO/build/firmware"
APPS=(launcher muse nevoflux)

mkdir -p "$OUT" "$REPO/build/idf"

# Optional baked-in fallback WiFi (only used when the `pendant` NVS namespace is empty).
WIFI_DEFAULTS="$REPO/build/idf/sdkconfig.wifi"
: > "$WIFI_DEFAULTS"
if [[ -n "${WIFI_SSID:-}" ]]; then
    printf 'CONFIG_PENDANT_WIFI_DEFAULT_SSID="%s"\nCONFIG_PENDANT_WIFI_DEFAULT_PASSWORD="%s"\n' \
        "$WIFI_SSID" "${WIFI_PASS:-}" > "$WIFI_DEFAULTS"
    echo "[i] Baking fallback WiFi SSID \"$WIFI_SSID\" into firmware"
fi

idf() {
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH=/project/build/idf/.component_cache \
        -v "$REPO":/project -w /project "$IDF_IMAGE" "$@"
}

for app in "${APPS[@]}"; do
    echo "=================== Building apps/$app ==================="
    bdir="build/idf/$app"
    # Fresh sdkconfig each time so sdkconfig.defaults changes always apply.
    rm -f "$REPO/$bdir/sdkconfig"
    idf idf.py -C "apps/$app" -B "$bdir" \
        -D SDKCONFIG="/project/$bdir/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="/project/apps/$app/sdkconfig.defaults;/project/build/idf/sdkconfig.wifi" \
        set-target esp32s3 build
done

echo "=================== Packing storage partition ==================="
python3 "$REPO/tools/pack_storage/pack_storage.py" --out "$REPO/build/storage.bin"

L="$REPO/build/idf/launcher"
cp "$L/bootloader/bootloader.bin"                "$OUT/bootloader.bin"
cp "$L/partition_table/partition-table.bin"      "$OUT/partition-table.bin"
cp "$L/ota_data_initial.bin"                     "$OUT/ota_data_initial.bin"
cp "$L/nevoflux_launcher.bin"                    "$OUT/nevoflux_launcher.bin"
cp "$REPO/build/idf/muse/muse_gadget_pendant.bin" "$OUT/muse_gadget_pendant.bin"
cp "$REPO/build/idf/nevoflux/nevoflux_pendant.bin" "$OUT/nevoflux_pendant.bin"
cp "$REPO/build/storage.bin"                     "$OUT/storage.bin"

cat > "$OUT/flash_args" <<'EOF'
--flash_mode dio --flash_freq 80m --flash_size 8MB
0x0 bootloader.bin
0x8000 partition-table.bin
0xf000 ota_data_initial.bin
0x20000 nevoflux_launcher.bin
0x1a0000 muse_gadget_pendant.bin
0x4a0000 nevoflux_pendant.bin
0x720000 storage.bin
EOF

# Partition fit check (sizes from partitions.csv)
check_fit() { # file max_bytes label
    local sz; sz=$(stat -c %s "$1")
    printf '  %-26s %8d / %8d bytes (%3d%%)\n' "$3" "$sz" "$2" $(( sz * 100 / $2 ))
    (( sz <= $2 )) || { echo "ERROR: $3 does not fit its partition"; exit 1; }
}
echo "Partition usage:"
check_fit "$OUT/nevoflux_launcher.bin"   $((0x180000)) "factory (launcher)"
check_fit "$OUT/muse_gadget_pendant.bin" $((0x300000)) "ota_0 (muse)"
check_fit "$OUT/nevoflux_pendant.bin"    $((0x280000)) "ota_1 (nevoflux)"
check_fit "$OUT/storage.bin"             $((0xE0000)) "storage"


echo "=================== Verifying & merging ==================="
idf bash -c 'cd /project/build/firmware && \
    for f in bootloader.bin nevoflux_launcher.bin muse_gadget_pendant.bin nevoflux_pendant.bin; do \
        esptool.py --chip esp32s3 image_info "$f" >/dev/null || { echo "invalid image: $f"; exit 1; }; done && \
    python -m esptool --chip esp32s3 merge_bin -o nevoflux-pendant-8MB.bin --fill-flash-size 8MB @flash_args'

( cd "$OUT" && sha256sum ./*.bin > SHA256SUMS )
echo
echo "Done. Flash with:"
echo "  esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x0 $OUT/nevoflux-pendant-8MB.bin"
