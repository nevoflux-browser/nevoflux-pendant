#include "muse_oauth_device.h"
#include "bsp_143c.h"
#include "prov_nvs.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_ota0";

static void reboot_to_launcher(void)
{
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    if (otadata) {
        (void)esp_partition_erase_range(otadata, 0U, otadata->size);
    }
    esp_restart();
}

static bool http_post_form(const char *url, const char *form_body, char *out_json, size_t out_cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .user_agent = "muse-gadget-sdk-esp32/1.0 (Waveshare-1.43C)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }
    (void)esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    size_t body_len = strlen(form_body);

    bool ok = false;
    if (esp_http_client_open(client, (int)body_len) == ESP_OK) {
        if (esp_http_client_write(client, form_body, (int)body_len) >= 0) {
            (void)esp_http_client_fetch_headers(client);
            int rd = esp_http_client_read_response(client, out_json, (int)out_cap - 1);
            if (rd >= 0) {
                out_json[rd] = '\0';
                ok = true;
            }
        }
        (void)esp_http_client_close(client);
    }
    (void)esp_http_client_cleanup(client);
    return ok;
}

void app_main(void)
{
    /* 1. Earliest boot-escape check: holding BOOT >= 2s returns to Launcher (factory) */
    if (bsp_143c_check_boot_escape(2000U)) {
        ESP_LOGW(TAG, "BOOT held >= 2s on startup; erasing otadata and returning to Launcher");
        reboot_to_launcher();
    }

    (void)prov_nvs_init();
    (void)bsp_143c_init();

    pendant_config_t pendant_cfg = {0};
    (void)prov_nvs_load_pendant(&pendant_cfg);

    muse_device_ctx_t muse_ctx;
    muse_device_init(&muse_ctx);

    if (!pendant_cfg.has_wifi) {
        bsp_143c_ui_set_status("Muse: 未配置 WiFi");
        bsp_143c_ui_set_transcript("请切回 Launcher 通过 BLE 或 USB 配置 WiFi");
    } else if (muse_ctx.auth_state == MUSE_AUTH_UNPAIRED) {
        /* Start OAuth 2.0 Device Authorization Grant (RFC 8628) */
        bsp_143c_ui_set_status("Muse OAuth2 设备授权");
        char req_body[192];
        char resp_json[1024];
        if (muse_oauth_build_device_code_req(&muse_ctx, req_body, sizeof(req_body)) > 0 &&
            http_post_form(MUSE_OAUTH_DEVICE_AUTH_URL, req_body, resp_json, sizeof(resp_json))) {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            if (muse_oauth_on_device_code_resp(&muse_ctx, resp_json, now_ms)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "请访问 %s\n输入授权码: %s\n(或直接扫描屏幕二维码)",
                         muse_ctx.verification_uri, muse_ctx.user_code);
                bsp_143c_ui_set_transcript(msg);
            }
        }
    } else {
        bsp_143c_ui_set_status("Muse AI 已连接");
        bsp_143c_ui_set_transcript("按住 BOOT 键与 Muse 语音对话");
    }

    for (;;) {
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if (muse_oauth_should_poll(&muse_ctx, now_ms)) {
            char poll_body[256];
            char poll_resp[1024];
            if (muse_oauth_build_token_poll_req(&muse_ctx, poll_body, sizeof(poll_body)) > 0 &&
                http_post_form(MUSE_OAUTH_TOKEN_URL, poll_body, poll_resp, sizeof(poll_resp))) {
                if (muse_oauth_on_token_poll_resp(&muse_ctx, poll_resp, now_ms)) {
                    ESP_LOGI(TAG, "RFC 8628 Device Flow authorized! Token saved to muse_cfg NVS");
                    bsp_143c_ui_set_status("Muse AI 已授权并连接");
                    bsp_143c_ui_set_transcript("授权成功！按住 BOOT 键开始与 Muse 对话");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
#endif /* ESP_PLATFORM */
