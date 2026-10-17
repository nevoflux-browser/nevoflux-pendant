#ifndef LAUNCHER_BLE_PROV_H
#define LAUNCHER_BLE_PROV_H

#include <stdbool.h>
#include "prov_nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize NimBLE GATT service ("NevoFlux-Pendant") in the Launcher
 * (`factory` partition). Incoming GATT writes are routed into the shared
 * `prov_rpc_ctx_t` (requiring physical BOOT button confirmation within 15s).
 */
bool launcher_ble_prov_init(prov_rpc_ctx_t *rpc_ctx);

#ifdef __cplusplus
}
#endif

#endif /* LAUNCHER_BLE_PROV_H */
