#ifndef PROV_NVS_H
#define PROV_NVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_NVS_NS_PENDANT         "pendant"
#define PROV_NVS_NS_NF_CFG          "nf_cfg"
#define PROV_NVS_NS_MUSE_CFG        "muse_cfg"

#define PROV_CONFIRM_TIMEOUT_MS     15000ULL

#define PROV_CHANNEL_ID_HEX_LEN     32U
#define PROV_CHANNEL_KEY_BYTES      32U

typedef enum {
    PENDANT_SYS_NEVOFLUX = 0,
    PENDANT_SYS_MUSE     = 1,
} pendant_system_t;

typedef struct {
    char             wifi_ssid[64];
    char             wifi_pass[64];
    pendant_system_t last_system;
    bool             has_wifi;
} pendant_config_t;

typedef struct {
    char    relay_url[128];
    char    account_token[256];
    char    control_channel_id[PROV_CHANNEL_ID_HEX_LEN + 1];
    uint8_t control_key[PROV_CHANNEL_KEY_BYTES];
    char    data_channel_id[PROV_CHANNEL_ID_HEX_LEN + 1];
    uint8_t data_key[PROV_CHANNEL_KEY_BYTES];
    bool    is_paired;
} nf_credentials_t;

typedef struct {
    char access_token[256];
    char refresh_token[256];
    char device_id[64];
    bool is_authorized;
} muse_credentials_t;

bool prov_nvs_init(void);
bool prov_nvs_load_pendant(pendant_config_t *out_cfg);
bool prov_nvs_save_pendant(const pendant_config_t *cfg);
bool prov_nvs_load_nf_cfg(nf_credentials_t *out_cred);
bool prov_nvs_save_nf_cfg(const nf_credentials_t *cred);
bool prov_nvs_clear_nf_cfg(void);
bool prov_nvs_load_muse_cfg(muse_credentials_t *out_cred);
bool prov_nvs_save_muse_cfg(const muse_credentials_t *cred);
bool prov_nvs_clear_muse_cfg(void);

/* =========================================================================
 * USB Serial / BLE JSON-RPC Provisioning State Machine with 15s BOOT Confirm
 * ========================================================================= */

typedef enum {
    PROV_STATE_IDLE         = 0,
    PROV_STATE_WAIT_CONFIRM = 1,
    PROV_STATE_CONFIRMED    = 2,
} prov_rpc_state_t;

typedef void (*prov_prompt_cb_t)(const char *device_name, uint32_t timeout_sec, void *user_ctx);
typedef void (*prov_applied_cb_t)(const pendant_config_t *pendant,
                                  const nf_credentials_t *cred,
                                  void *user_ctx);

typedef struct {
    prov_rpc_state_t  state;
    uint64_t          offer_timestamp_ms;
    char              pending_device[64];
    prov_prompt_cb_t  on_prompt;
    prov_applied_cb_t on_applied;
    void             *user_ctx;
} prov_rpc_ctx_t;

void prov_rpc_init(prov_rpc_ctx_t *ctx,
                   prov_prompt_cb_t on_prompt,
                   prov_applied_cb_t on_applied,
                   void *user_ctx);

/**
 * Called by button handler when the user physically clicks BOOT during the 15s window.
 */
bool prov_rpc_confirm_boot(prov_rpc_ctx_t *ctx, uint64_t now_ms);

/**
 * Process a single JSON-RPC line from USB Serial or BLE GATT.
 * Writes JSON response into `resp_buf`.
 */
int prov_rpc_handle_line(prov_rpc_ctx_t *ctx,
                         const char *json_line,
                         uint64_t now_ms,
                         char *resp_buf,
                         size_t resp_cap);

bool prov_hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out_bytes, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* PROV_NVS_H */
