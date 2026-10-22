#ifndef PENDANT_WIFI_H
#define PENDANT_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PENDANT_WIFI_OFF = 0,        /* no SSID configured */
    PENDANT_WIFI_CONNECTING,
    PENDANT_WIFI_CONNECTED,      /* associated and got an IPv4 address */
    PENDANT_WIFI_FAILED,         /* last attempt failed; retrying with backoff */
} pendant_wifi_state_t;

typedef struct {
    pendant_wifi_state_t state;
    char     ssid[33];
    char     ip[16];
    int8_t   rssi;
    uint8_t  last_reason;        /* wifi_err_reason_t of the last disconnect */
} pendant_wifi_status_t;

/**
 * Start WiFi STA and keep it connected (auto-reconnect with backoff).
 * ssid == NULL or "" falls back to CONFIG_PENDANT_WIFI_DEFAULT_SSID.
 * Requires nvs_flash_init() to have been called. Safe to call again with new
 * credentials (reconnects).
 */
bool pendant_wifi_start(const char *ssid, const char *password);

void pendant_wifi_get_status(pendant_wifi_status_t *out);

/** Block until connected or timeout. */
bool pendant_wifi_wait_connected(uint32_t timeout_ms);

/** Human readable one-line status for the screen, e.g. "WiFi: MyAP · 192.168.1.23". */
void pendant_wifi_format_line(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PENDANT_WIFI_H */
