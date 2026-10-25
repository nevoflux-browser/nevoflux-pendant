#include "prov_nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int  prompt_count;
    int  applied_count;
    char last_device[64];
} test_cb_ctx_t;

static void on_prompt(const char *device_name, uint32_t timeout_sec, void *user_ctx)
{
    (void)timeout_sec;
    test_cb_ctx_t *c = (test_cb_ctx_t *)user_ctx;
    c->prompt_count++;
    strncpy(c->last_device, device_name, sizeof(c->last_device) - 1U);
}

static void on_applied(const pendant_config_t *pendant, const nf_credentials_t *cred, void *user_ctx)
{
    (void)pendant;
    (void)cred;
    test_cb_ctx_t *c = (test_cb_ctx_t *)user_ctx;
    c->applied_count++;
}

int main(void)
{
    assert(prov_nvs_init());

    test_cb_ctx_t cb_ctx = {0};
    prov_rpc_ctx_t rpc;
    prov_rpc_init(&rpc, on_prompt, on_applied, &cb_ctx);

    char resp[256];
    const char *write_cmd =
        "{\"cmd\":\"prov.write\","
        "\"wifi_ssid\":\"MyHotspot\","
        "\"wifi_pass\":\"secret123\","
        "\"relay_url\":\"wss://relay.nevoflux.app\","
        "\"account_token\":\"sess_tok_independent_7d\","
        "\"control_channel_id\":\"2f1c4a90-7b3e-4d1a-9c58-0e6a2b7d4f31\","
        "\"control_key_hex\":\"1111111111111111111111111111111111111111111111111111111111111111\","
        "\"data_channel_id\":\"9a8b7c6d-1111-4d1a-9c58-0e6a2b7d4f31\","
        "\"data_key_hex\":\"2222222222222222222222222222222222222222222222222222222222222222\"}";

    /* 1. Attempting prov.write without prov.offer + BOOT confirm must be rejected */
    assert(prov_rpc_handle_line(&rpc, write_cmd, 1000ULL, resp, sizeof(resp)) > 0);
    assert(strstr(resp, "boot_confirm_required") != NULL);

    /* 2. Send prov.offer -> transitions to WAIT_CONFIRM and triggers UI prompt */
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.offer\",\"device\":\"Sam-ThinkPad\"}", 1000ULL, resp, sizeof(resp)) > 0);
    assert(strstr(resp, "\"status\":\"wait_boot\"") != NULL);
    assert(cb_ctx.prompt_count == 1);
    assert(strcmp(cb_ctx.last_device, "Sam-ThinkPad") == 0);

    /* 3. Still rejected if BOOT has not been pressed yet */
    assert(prov_rpc_handle_line(&rpc, write_cmd, 2000ULL, resp, sizeof(resp)) > 0);
    assert(strstr(resp, "boot_confirm_required") != NULL);

    /* 4. Simulate 15s timeout expiry (1000 + 16000 = 17000ms) -> BOOT press after timeout fails */
    assert(!prov_rpc_confirm_boot(&rpc, 17000ULL));

    /* 5. Re-offer at t=20000ms, press BOOT at t=23000ms -> status becomes "ready" */
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.offer\",\"device\":\"Sam-ThinkPad\"}", 20000ULL, resp, sizeof(resp)) > 0);
    assert(prov_rpc_confirm_boot(&rpc, 23000ULL));
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.status\"}", 23500ULL, resp, sizeof(resp)) > 0);
    assert(strcmp(resp, "{\"status\":\"ready\"}") == 0);

    /* 6. Now prov.write succeeds and persists both pendant WiFi and nf_cfg credentials */
    assert(prov_rpc_handle_line(&rpc, write_cmd, 24000ULL, resp, sizeof(resp)) > 0);
    assert(strcmp(resp, "{\"status\":\"ok\"}") == 0);
    assert(cb_ctx.applied_count == 1);

    pendant_config_t loaded_pendant;
    assert(prov_nvs_load_pendant(&loaded_pendant));
    assert(loaded_pendant.has_wifi);
    assert(strcmp(loaded_pendant.wifi_ssid, "MyHotspot") == 0);
    assert(strcmp(loaded_pendant.wifi_pass, "secret123") == 0);

    nf_credentials_t loaded_cred;
    assert(prov_nvs_load_nf_cfg(&loaded_cred));
    assert(loaded_cred.is_paired);
    assert(strcmp(loaded_cred.account_token, "sess_tok_independent_7d") == 0);
    assert(strcmp(loaded_cred.data_channel_id, "9a8b7c6d-1111-4d1a-9c58-0e6a2b7d4f31") == 0);
    assert(loaded_cred.data_key[0] == 0x22U && loaded_cred.data_key[31] == 0x22U);

    /* 7. Verify Muse RFC 8628 credential NVS persistence (muse_cfg namespace) */
    muse_credentials_t muse_cred = {0};
    assert(!prov_nvs_load_muse_cfg(&muse_cred));
    snprintf(muse_cred.access_token, sizeof(muse_cred.access_token), "mgst_oauth2_device_token_xyz");
    snprintf(muse_cred.refresh_token, sizeof(muse_cred.refresh_token), "mgrt_refresh_token_123");
    snprintf(muse_cred.device_id, sizeof(muse_cred.device_id), "MuseGadget-A1B2C3");
    assert(prov_nvs_save_muse_cfg(&muse_cred));

    muse_credentials_t loaded_muse = {0};
    assert(prov_nvs_load_muse_cfg(&loaded_muse));
    assert(loaded_muse.is_authorized);
    assert(strcmp(loaded_muse.access_token, "mgst_oauth2_device_token_xyz") == 0);
    assert(strcmp(loaded_muse.refresh_token, "mgrt_refresh_token_123") == 0);
    assert(strcmp(loaded_muse.device_id, "MuseGadget-A1B2C3") == 0);

    assert(prov_nvs_clear_muse_cfg());
    assert(!prov_nvs_load_muse_cfg(&loaded_muse));

    /* 8. prov.wifi (WiFi-only) needs BOOT confirm, updates WiFi (incl. UTF-8 SSID), keeps nf_cfg */
    const char *wifi_cmd = "{\"cmd\":\"prov.wifi\",\"wifi_ssid\":\"家里的WiFi\",\"wifi_pass\":\"p@ss \\\"q\\\"\"}";
    assert(prov_rpc_handle_line(&rpc, wifi_cmd, 30000ULL, resp, sizeof(resp)) > 0);
    assert(strstr(resp, "boot_confirm_required") != NULL);
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.offer\",\"device\":\"PC\"}", 30000ULL, resp, sizeof(resp)) > 0);
    assert(prov_rpc_confirm_boot(&rpc, 31000ULL));
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.wifi\"}", 31500ULL, resp, sizeof(resp)) > 0);
    assert(strstr(resp, "missing_wifi_ssid") != NULL);
    assert(prov_rpc_handle_line(&rpc, wifi_cmd, 32000ULL, resp, sizeof(resp)) > 0);
    assert(strcmp(resp, "{\"status\":\"ok\"}") == 0);
    assert(cb_ctx.applied_count == 2);
    assert(prov_nvs_load_pendant(&loaded_pendant));
    assert(strcmp(loaded_pendant.wifi_ssid, "家里的WiFi") == 0);
    assert(strcmp(loaded_pendant.wifi_pass, "p@ss \"q\"") == 0);
    assert(prov_nvs_load_nf_cfg(&loaded_cred) && loaded_cred.is_paired);
    assert(prov_rpc_handle_line(&rpc, "{\"cmd\":\"prov.status\"}", 32500ULL, resp, sizeof(resp)) > 0);
    assert(strcmp(resp, "{\"status\":\"idle\"}") == 0);

    printf("[PASS] All prov_nvs unit tests passed!\n");
    return 0;
}

