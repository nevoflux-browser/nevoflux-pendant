#ifndef LAUNCHER_BLE_PROV_H
#define LAUNCHER_BLE_PROV_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE provisioning transport for the Launcher (`factory` partition).
 *
 * Exposes the Nordic UART Service (NUS) so any BLE UART client (nRF Connect,
 * `provision.py --port ble`) can speak the same newline-delimited JSON-RPC as
 * the USB console (`prov.offer` -> BOOT click within 15s -> `prov.wifi` /
 * `prov.write`). The GATT callbacks only queue complete lines; they are
 * handled by the main loop so `prov_rpc_ctx_t` stays single-threaded.
 *
 *   Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX       6E400002-...  write / write-without-response  (client -> pendant)
 *   TX       6E400003-...  notify                          (pendant -> client)
 *
 * Advertised name: "NevoFlux-XXXX" (last two bytes of the BT MAC).
 *
 * The stack is initialised at boot but stays invisible: it only advertises
 * after launcher_ble_prov_set_enabled(true) (user taps "开启蓝牙配网").
 */
bool launcher_ble_prov_init(void);

/* Start/stop advertising. Disabling also drops a connected client. */
void launcher_ble_prov_set_enabled(bool enabled);
bool launcher_ble_prov_is_enabled(void);

/* True while a BLE client is connected. */
bool launcher_ble_prov_is_connected(void);

/* Non-blocking: copies one complete request line (NUL-terminated) into `out`. */
bool launcher_ble_prov_poll(char *out, size_t cap);

/* Notifies `resp` + '\n' to the connected client (chunked to the ATT MTU). */
void launcher_ble_prov_send(const char *resp);

/* Advertised device name, e.g. "NevoFlux-1A2B" ("" before init). */
const char *launcher_ble_prov_name(void);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_BLE_PROV_H */
