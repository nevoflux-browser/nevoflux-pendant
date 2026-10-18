#include "prov_nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "nvs.h"
#include "nvs_flash.h"
#endif

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool prov_hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out_bytes, size_t out_len)
{
    if (!hex || !out_bytes || hex_len != out_len * 2U) {
        return false;
    }
    for (size_t i = 0U; i < out_len; ++i) {
        int hi = hex_nibble(hex[i * 2U]);
        int lo = hex_nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out_bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool is_valid_hex_str(const char *s, size_t expected_len)
{
    if (!s || strlen(s) != expected_len) {
        return false;
    }
    for (size_t i = 0U; i < expected_len; ++i) {
        if (hex_nibble(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

#if defined(ESP_PLATFORM)

bool prov_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) {
            return false;
        }
        ret = nvs_flash_init();
    }
    return (ret == ESP_OK);
}

bool prov_nvs_load_pendant(pendant_config_t *out_cfg)
{
    if (!out_cfg) {
        return false;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->last_system = PENDANT_SYS_NEVOFLUX;

    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_PENDANT, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(out_cfg->wifi_ssid);
    size_t pass_len = sizeof(out_cfg->wifi_pass);
    uint8_t last_sys = 0U;

    if (nvs_get_str(h, "wifi_ssid", out_cfg->wifi_ssid, &ssid_len) == ESP_OK &&
        out_cfg->wifi_ssid[0] != '\0') {
        out_cfg->has_wifi = true;
    }
    (void)nvs_get_str(h, "wifi_pass", out_cfg->wifi_pass, &pass_len);
    if (nvs_get_u8(h, "last_system", &last_sys) == ESP_OK) {
        out_cfg->last_system = (last_sys == 1U) ? PENDANT_SYS_MUSE : PENDANT_SYS_NEVOFLUX;
    }
    nvs_close(h);
    return true;
}

bool prov_nvs_save_pendant(const pendant_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_PENDANT, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid) == ESP_OK) &&
              (nvs_set_str(h, "wifi_pass", cfg->wifi_pass) == ESP_OK) &&
              (nvs_set_u8(h, "last_system", (uint8_t)cfg->last_system) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool prov_nvs_load_nf_cfg(nf_credentials_t *out_cred)
{
    if (!out_cred) {
        return false;
    }
    memset(out_cred, 0, sizeof(*out_cred));
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_NF_CFG, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t url_len = sizeof(out_cred->relay_url);
    size_t tok_len = sizeof(out_cred->account_token);
    size_t ctrl_id_len = sizeof(out_cred->control_channel_id);
    size_t data_id_len = sizeof(out_cred->data_channel_id);
    size_t ctrl_key_len = PROV_CHANNEL_KEY_BYTES;
    size_t data_key_len = PROV_CHANNEL_KEY_BYTES;

    bool ok = (nvs_get_str(h, "relay_url", out_cred->relay_url, &url_len) == ESP_OK) &&
              (nvs_get_str(h, "account_token", out_cred->account_token, &tok_len) == ESP_OK) &&
              (nvs_get_str(h, "data_cid", out_cred->data_channel_id, &data_id_len) == ESP_OK) &&
              (nvs_get_blob(h, "data_key", out_cred->data_key, &data_key_len) == ESP_OK &&
               data_key_len == PROV_CHANNEL_KEY_BYTES);

    (void)nvs_get_str(h, "ctrl_cid", out_cred->control_channel_id, &ctrl_id_len);
    (void)nvs_get_blob(h, "ctrl_key", out_cred->control_key, &ctrl_key_len);
    nvs_close(h);

    out_cred->is_paired = ok && (out_cred->account_token[0] != '\0');
    return out_cred->is_paired;
}

bool prov_nvs_save_nf_cfg(const nf_credentials_t *cred)
{
    if (!cred) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_NF_CFG, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_str(h, "relay_url", cred->relay_url) == ESP_OK) &&
              (nvs_set_str(h, "account_token", cred->account_token) == ESP_OK) &&
              (nvs_set_str(h, "ctrl_cid", cred->control_channel_id) == ESP_OK) &&
              (nvs_set_blob(h, "ctrl_key", cred->control_key, PROV_CHANNEL_KEY_BYTES) == ESP_OK) &&
              (nvs_set_str(h, "data_cid", cred->data_channel_id) == ESP_OK) &&
              (nvs_set_blob(h, "data_key", cred->data_key, PROV_CHANNEL_KEY_BYTES) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool prov_nvs_clear_nf_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_NF_CFG, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_erase_all(h) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool prov_nvs_load_muse_cfg(muse_credentials_t *out_cred)
{
    if (!out_cred) {
        return false;
    }
    memset(out_cred, 0, sizeof(*out_cred));
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_MUSE_CFG, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t acc_len = sizeof(out_cred->access_token);
    size_t ref_len = sizeof(out_cred->refresh_token);
    size_t dev_len = sizeof(out_cred->device_id);

    bool ok = (nvs_get_str(h, "access_token", out_cred->access_token, &acc_len) == ESP_OK) &&
              (out_cred->access_token[0] != '\0');
    (void)nvs_get_str(h, "refresh_token", out_cred->refresh_token, &ref_len);
    (void)nvs_get_str(h, "device_id", out_cred->device_id, &dev_len);
    nvs_close(h);

    out_cred->is_authorized = ok;
    return out_cred->is_authorized;
}

bool prov_nvs_save_muse_cfg(const muse_credentials_t *cred)
{
    if (!cred || cred->access_token[0] == '\0') {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_MUSE_CFG, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_str(h, "access_token", cred->access_token) == ESP_OK) &&
              (nvs_set_str(h, "refresh_token", cred->refresh_token) == ESP_OK) &&
              (nvs_set_str(h, "device_id", cred->device_id) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool prov_nvs_clear_muse_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS_MUSE_CFG, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_erase_all(h) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

#else /* Host mock storage for unit testing */

static pendant_config_t   s_mock_pendant;
static nf_credentials_t   s_mock_nf_cred;
static muse_credentials_t s_mock_muse_cred;

bool prov_nvs_init(void)
{
    memset(&s_mock_pendant, 0, sizeof(s_mock_pendant));
    memset(&s_mock_nf_cred, 0, sizeof(s_mock_nf_cred));
    memset(&s_mock_muse_cred, 0, sizeof(s_mock_muse_cred));
    return true;
}

bool prov_nvs_load_pendant(pendant_config_t *out_cfg)
{
    if (!out_cfg) {
        return false;
    }
    *out_cfg = s_mock_pendant;
    return true;
}

bool prov_nvs_save_pendant(const pendant_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    s_mock_pendant = *cfg;
    s_mock_pendant.has_wifi = (cfg->wifi_ssid[0] != '\0');
    return true;
}

bool prov_nvs_load_nf_cfg(nf_credentials_t *out_cred)
{
    if (!out_cred) {
        return false;
    }
    *out_cred = s_mock_nf_cred;
    return s_mock_nf_cred.is_paired;
}

bool prov_nvs_save_nf_cfg(const nf_credentials_t *cred)
{
    if (!cred) {
        return false;
    }
    s_mock_nf_cred = *cred;
    s_mock_nf_cred.is_paired = true;
    return true;
}

bool prov_nvs_clear_nf_cfg(void)
{
    memset(&s_mock_nf_cred, 0, sizeof(s_mock_nf_cred));
    return true;
}

bool prov_nvs_load_muse_cfg(muse_credentials_t *out_cred)
{
    if (!out_cred) {
        return false;
    }
    *out_cred = s_mock_muse_cred;
    return s_mock_muse_cred.is_authorized;
}

bool prov_nvs_save_muse_cfg(const muse_credentials_t *cred)
{
    if (!cred || cred->access_token[0] == '\0') {
        return false;
    }
    s_mock_muse_cred = *cred;
    s_mock_muse_cred.is_authorized = true;
    return true;
}

bool prov_nvs_clear_muse_cfg(void)
{
    memset(&s_mock_muse_cred, 0, sizeof(s_mock_muse_cred));
    return true;
}

#endif /* ESP_PLATFORM */

/* =========================================================================
 * Lightweight allocation-free JSON string field extractor
 * ========================================================================= */

static bool json_extract_str(const char *json, const char *key, char *out_val, size_t out_cap)
{
    if (!json || !key || !out_val || out_cap == 0U) {
        return false;
    }
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        return false;
    }
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += (size_t)n;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p != ':') {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p != '"') {
        return false;
    }
    ++p;

    size_t i = 0U;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && *(p + 1) != '\0') {
            ++p;
        }
        if (i + 1U >= out_cap) {
            return false;
        }
        out_val[i++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out_val[i] = '\0';
    return true;
}

void prov_rpc_init(prov_rpc_ctx_t *ctx,
                   prov_prompt_cb_t on_prompt,
                   prov_applied_cb_t on_applied,
                   void *user_ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = PROV_STATE_IDLE;
    ctx->on_prompt = on_prompt;
    ctx->on_applied = on_applied;
    ctx->user_ctx = user_ctx;
}

static void check_confirm_expiry(prov_rpc_ctx_t *ctx, uint64_t now_ms)
{
    if (ctx->state != PROV_STATE_IDLE &&
        now_ms > ctx->offer_timestamp_ms &&
        (now_ms - ctx->offer_timestamp_ms) > PROV_CONFIRM_TIMEOUT_MS) {
        ctx->state = PROV_STATE_IDLE;
        ctx->pending_device[0] = '\0';
    }
}

bool prov_rpc_confirm_boot(prov_rpc_ctx_t *ctx, uint64_t now_ms)
{
    if (!ctx) {
        return false;
    }
    check_confirm_expiry(ctx, now_ms);
    if (ctx->state != PROV_STATE_WAIT_CONFIRM) {
        return false;
    }
    ctx->state = PROV_STATE_CONFIRMED;
    ctx->offer_timestamp_ms = now_ms; /* extend 15s window for actual write */
    return true;
}

int prov_rpc_handle_line(prov_rpc_ctx_t *ctx,
                         const char *json_line,
                         uint64_t now_ms,
                         char *resp_buf,
                         size_t resp_cap)
{
    if (!ctx || !json_line || !resp_buf || resp_cap == 0U) {
        return -1;
    }
    check_confirm_expiry(ctx, now_ms);

    char cmd[32];
    if (!json_extract_str(json_line, "cmd", cmd, sizeof(cmd))) {
        return snprintf(resp_buf, resp_cap, "{\"status\":\"error\",\"reason\":\"missing_cmd\"}");
    }

    if (strcmp(cmd, "prov.offer") == 0) {
        if (!json_extract_str(json_line, "device", ctx->pending_device, sizeof(ctx->pending_device))) {
            strncpy(ctx->pending_device, "Unknown-PC", sizeof(ctx->pending_device) - 1U);
        }
        ctx->state = PROV_STATE_WAIT_CONFIRM;
        ctx->offer_timestamp_ms = now_ms;
        if (ctx->on_prompt) {
            ctx->on_prompt(ctx->pending_device, (uint32_t)(PROV_CONFIRM_TIMEOUT_MS / 1000ULL), ctx->user_ctx);
        }
        return snprintf(resp_buf, resp_cap,
                        "{\"status\":\"wait_boot\",\"timeout_s\":15,\"device\":\"%s\"}",
                        ctx->pending_device);
    }

    if (strcmp(cmd, "prov.status") == 0) {
        const char *st = (ctx->state == PROV_STATE_CONFIRMED)    ? "ready"
                       : (ctx->state == PROV_STATE_WAIT_CONFIRM) ? "wait_boot"
                                                                 : "idle";
        return snprintf(resp_buf, resp_cap, "{\"status\":\"%s\"}", st);
    }

    if (strcmp(cmd, "prov.write") == 0) {
        if (ctx->state != PROV_STATE_CONFIRMED) {
            return snprintf(resp_buf, resp_cap,
                            "{\"status\":\"error\",\"reason\":\"boot_confirm_required\"}");
        }

        nf_credentials_t cred;
        memset(&cred, 0, sizeof(cred));
        if (!json_extract_str(json_line, "relay_url", cred.relay_url, sizeof(cred.relay_url)) ||
            cred.relay_url[0] == '\0') {
            strncpy(cred.relay_url, "wss://relay.nevoflux.app", sizeof(cred.relay_url) - 1U);
        }

        char ctrl_key_hex[65];
        char data_key_hex[65];
        if (!json_extract_str(json_line, "account_token", cred.account_token, sizeof(cred.account_token)) ||
            cred.account_token[0] == '\0' ||
            !json_extract_str(json_line, "control_channel_id", cred.control_channel_id, sizeof(cred.control_channel_id)) ||
            !is_valid_hex_str(cred.control_channel_id, PROV_CHANNEL_ID_HEX_LEN) ||
            !json_extract_str(json_line, "control_key_hex", ctrl_key_hex, sizeof(ctrl_key_hex)) ||
            !prov_hex_to_bytes(ctrl_key_hex, strlen(ctrl_key_hex), cred.control_key, PROV_CHANNEL_KEY_BYTES) ||
            !json_extract_str(json_line, "data_channel_id", cred.data_channel_id, sizeof(cred.data_channel_id)) ||
            !is_valid_hex_str(cred.data_channel_id, PROV_CHANNEL_ID_HEX_LEN) ||
            !json_extract_str(json_line, "data_key_hex", data_key_hex, sizeof(data_key_hex)) ||
            !prov_hex_to_bytes(data_key_hex, strlen(data_key_hex), cred.data_key, PROV_CHANNEL_KEY_BYTES)) {
            return snprintf(resp_buf, resp_cap,
                            "{\"status\":\"error\",\"reason\":\"invalid_credential_fields\"}");
        }
        cred.is_paired = true;

        pendant_config_t pendant;
        (void)prov_nvs_load_pendant(&pendant);
        char ssid[sizeof(pendant.wifi_ssid)];
        if (json_extract_str(json_line, "wifi_ssid", ssid, sizeof(ssid)) && ssid[0] != '\0') {
            snprintf(pendant.wifi_ssid, sizeof(pendant.wifi_ssid), "%s", ssid);
            pendant.has_wifi = true;
            char pass[sizeof(pendant.wifi_pass)];
            if (json_extract_str(json_line, "wifi_pass", pass, sizeof(pass))) {
                snprintf(pendant.wifi_pass, sizeof(pendant.wifi_pass), "%s", pass);
            }
            if (!prov_nvs_save_pendant(&pendant)) {
                return snprintf(resp_buf, resp_cap, "{\"status\":\"error\",\"reason\":\"nvs_pendant_save_failed\"}");
            }
        }

        if (!prov_nvs_save_nf_cfg(&cred)) {
            return snprintf(resp_buf, resp_cap, "{\"status\":\"error\",\"reason\":\"nvs_nf_cfg_save_failed\"}");
        }

        ctx->state = PROV_STATE_IDLE;
        if (ctx->on_applied) {
            ctx->on_applied(&pendant, &cred, ctx->user_ctx);
        }
        return snprintf(resp_buf, resp_cap, "{\"status\":\"ok\"}");
    }

    return snprintf(resp_buf, resp_cap, "{\"status\":\"error\",\"reason\":\"unknown_cmd\"}");
}
