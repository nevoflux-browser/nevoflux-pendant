#include "ble_prov.h"
#include <stddef.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

static prov_rpc_ctx_t *s_ble_rpc_ctx = NULL;

bool launcher_ble_prov_init(prov_rpc_ctx_t *rpc_ctx)
{
    if (!rpc_ctx) {
        return false;
    }
    s_ble_rpc_ctx = rpc_ctx;
#if defined(ESP_PLATFORM)
    ESP_LOGI("ble_prov", "NimBLE GATT provisioning service initialized (Launcher factory only)");
#endif
    return true;
}
