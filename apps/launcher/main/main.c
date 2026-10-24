#include <stdio.h>
#include <string.h>
#include "bsp_143c.h"
#include "prov_nvs.h"
#include "ble_prov.h"
#include "pendant_wifi.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char k_idle_prompt[] = "点选系统启动，或连接 USB 配网";
static char s_ui_prompt[128] = "点选系统启动，或连接 USB 配网";
static bool s_prov_applied;

/* BLE provisioning is invisible until the user opens it on the Launcher screen */
#define BLE_PROV_WINDOW_MS   (3U * 60U * 1000U)
#define BLE_PROV_LINGER_MS   1500U /* let the final "ok" notification go out before dropping the link */

static uint64_t now_ms(void)
{
#if defined(ESP_PLATFORM)
    return (uint64_t)(esp_timer_get_time() / 1000LL);
#else
    return 0ULL;
#endif
}

static void on_prov_prompt(const char *device_name, uint32_t timeout_sec, void *user_ctx)
{
    (void)user_ctx;
    snprintf(s_ui_prompt, sizeof(s_ui_prompt),
             "收到 [%s] 配对请求，请 %us 内单击 BOOT 确认",
             device_name ? device_name : "PC",
             (unsigned)timeout_sec);
}

static void on_prov_applied(const pendant_config_t *pendant,
                            const nf_credentials_t *cred,
                            void *user_ctx)
{
    (void)cred;
    (void)user_ctx;
    s_prov_applied = true;
    snprintf(s_ui_prompt, sizeof(s_ui_prompt), "配置已写入！正在连接 WiFi…");
#if defined(ESP_PLATFORM)
    if (pendant && pendant->has_wifi) {
        (void)pendant_wifi_start(pendant->wifi_ssid, pendant->wifi_pass);
    }
#else
    (void)pendant;
#endif
}

static void ble_open_prompt(void)
{
    snprintf(s_ui_prompt, sizeof(s_ui_prompt), "蓝牙配网已开启：%s\n3 分钟后自动关闭",
             launcher_ble_prov_name()[0] ? launcher_ble_prov_name() : "NevoFlux-…");
}

static bool boot_into_partition(pendant_system_t target_sys, pendant_config_t *pendant_cfg)
{
    pendant_cfg->last_system = target_sys;
    (void)prov_nvs_save_pendant(pendant_cfg);

#if defined(ESP_PLATFORM)
    esp_partition_subtype_t subtype = (target_sys == PENDANT_SYS_MUSE)
                                          ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                          : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, NULL);
    if (!part) {
        snprintf(s_ui_prompt, sizeof(s_ui_prompt), "错误：未找到目标 OTA 分区");
        return false;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        snprintf(s_ui_prompt, sizeof(s_ui_prompt), "错误：写入 otadata 失败");
        return false;
    }
    esp_restart();
#endif
    return true;
}

void app_main(void)
{
    (void)prov_nvs_init();
    (void)bsp_display_init();

    pendant_config_t pendant_cfg;
    (void)prov_nvs_load_pendant(&pendant_cfg);
#if defined(ESP_PLATFORM)
    (void)prov_usb_line_init();
    (void)pendant_wifi_start(pendant_cfg.has_wifi ? pendant_cfg.wifi_ssid : NULL, pendant_cfg.wifi_pass);
    uint64_t last_net_ms = 0ULL;
#endif

    nf_credentials_t nf_cred;
    (void)prov_nvs_load_nf_cfg(&nf_cred);

    prov_rpc_ctx_t rpc_ctx;
    prov_rpc_init(&rpc_ctx, on_prov_prompt, on_prov_applied, NULL);
    const bool ble_ok = launcher_ble_prov_init();
    bsp_display_set_ble_button(ble_ok, false);
    uint64_t ble_close_at_ms = 0ULL; /* 0 = BLE provisioning closed */
    bool ble_was_connected = false;

    bsp_key_fsm_t key_fsm;
    bsp_key_init(&key_fsm);

    char line_buf[1024];
    char resp_buf[256];

    for (;;) {
        uint64_t t_ms = now_ms();
        bsp_key_event_t kev = bsp_key_poll(&key_fsm, t_ms);

        if (kev == BSP_KEY_EVENT_BOOT_SINGLE_CLICK) {
            if (prov_rpc_confirm_boot(&rpc_ctx, t_ms)) {
                snprintf(s_ui_prompt, sizeof(s_ui_prompt), "已按键确认，等待电脑/手机写入配置…");
            } else {
                /* When not confirming pairing, single click toggles highlighted system */
                pendant_cfg.last_system = (pendant_cfg.last_system == PENDANT_SYS_NEVOFLUX)
                                              ? PENDANT_SYS_MUSE
                                              : PENDANT_SYS_NEVOFLUX;
            }
        } else if (kev == BSP_KEY_EVENT_BOOT_PRESS_START) {
            /* Hold BOOT in Launcher -> boot into currently highlighted system */
            (void)boot_into_partition(pendant_cfg.last_system, &pendant_cfg);
        }

        bsp_ui_touch_action_t touch = bsp_display_poll_touch();
        if (touch == BSP_UI_TOUCH_BOOT_MUSE) {
            (void)boot_into_partition(PENDANT_SYS_MUSE, &pendant_cfg);
        } else if (touch == BSP_UI_TOUCH_BOOT_NEVO) {
            (void)boot_into_partition(PENDANT_SYS_NEVOFLUX, &pendant_cfg);
        } else if (touch == BSP_UI_TOUCH_TOGGLE_BLE && ble_ok) {
            if (ble_close_at_ms == 0ULL) {
                launcher_ble_prov_set_enabled(true);
                ble_close_at_ms = t_ms + BLE_PROV_WINDOW_MS;
                ble_open_prompt();
            } else {
                ble_close_at_ms = 0ULL;
                launcher_ble_prov_set_enabled(false);
                snprintf(s_ui_prompt, sizeof(s_ui_prompt), "%s", k_idle_prompt);
            }
            bsp_display_set_ble_button(true, ble_close_at_ms != 0ULL);
        }

        /* Non-blocking USB Serial JSON-RPC line check */
#if defined(ESP_PLATFORM)
        if (prov_usb_line_poll(line_buf, sizeof(line_buf))) {
#else
        if (fgets(line_buf, sizeof(line_buf), stdin) != NULL) {
#endif
            if (prov_rpc_handle_line(&rpc_ctx, line_buf, t_ms, resp_buf, sizeof(resp_buf)) > 0) {
                printf("%s\n", resp_buf);
                fflush(stdout);
                (void)prov_nvs_load_pendant(&pendant_cfg);
                (void)prov_nvs_load_nf_cfg(&nf_cred);
            }
        }

        /* Same JSON-RPC over BLE (Nordic UART Service); replies go back as notifications */
        if (launcher_ble_prov_poll(line_buf, sizeof(line_buf))) {
            s_prov_applied = false;
            if (prov_rpc_handle_line(&rpc_ctx, line_buf, t_ms, resp_buf, sizeof(resp_buf)) > 0) {
                launcher_ble_prov_send(resp_buf);
                (void)prov_nvs_load_pendant(&pendant_cfg);
                (void)prov_nvs_load_nf_cfg(&nf_cred);
            }
            if (s_prov_applied && ble_close_at_ms != 0ULL) {
                ble_close_at_ms = t_ms + BLE_PROV_LINGER_MS; /* done: close shortly after replying */
            }
        }
        /* Show BLE link state so the user can tell the PC/phone actually reached the pendant */
        const bool ble_connected = launcher_ble_prov_is_connected();
        if (ble_connected != ble_was_connected) {
            ble_was_connected = ble_connected;
            if (ble_connected) {
                snprintf(s_ui_prompt, sizeof(s_ui_prompt), "蓝牙已连接，等待配网请求…");
            } else if (ble_close_at_ms != 0ULL && !s_prov_applied) {
                ble_open_prompt(); /* client left, window still open */
            }
        }
        if (ble_close_at_ms != 0ULL && t_ms >= ble_close_at_ms) {
            ble_close_at_ms = 0ULL;
            launcher_ble_prov_set_enabled(false);
            bsp_display_set_ble_button(true, false);
            if (!s_prov_applied) {
                snprintf(s_ui_prompt, sizeof(s_ui_prompt), "蓝牙配网已超时关闭");
            }
        }

#if defined(ESP_PLATFORM)
        if (t_ms - last_net_ms >= 500ULL) {
            char net_line[96];
            pendant_wifi_format_line(net_line, sizeof(net_line));
            bsp_display_set_net_info(net_line);
            last_net_ms = t_ms;
        }
#endif
        bsp_display_render_launcher(
            (int)pendant_cfg.last_system,
            pendant_cfg.has_wifi ? pendant_cfg.wifi_ssid : NULL,
            nf_cred.is_paired,
            s_ui_prompt);

#if defined(ESP_PLATFORM)
        vTaskDelay(pdMS_TO_TICKS(20));
#else
        break;
#endif
    }
}
