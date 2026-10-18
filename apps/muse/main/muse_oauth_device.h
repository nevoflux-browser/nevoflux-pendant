#ifndef MUSE_OAUTH_DEVICE_H
#define MUSE_OAUTH_DEVICE_H

#include "prov_nvs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OAuth 2.0 Device Authorization Grant (RFC 8628) endpoints for Muse Gadget SDK.
 */
#define MUSE_OAUTH_DEVICE_AUTH_URL  "https://gadgets.muse.ai/oauth/device/code"
#define MUSE_OAUTH_TOKEN_URL        "https://gadgets.muse.ai/oauth/token"
#define MUSE_OAUTH_DEFAULT_VERIFY   "https://muse.ai/link"
#define MUSE_OAUTH_CLIENT_ID        "muse-gadget-esp32-s3-143c"

typedef enum {
    MUSE_AUTH_UNPAIRED          = 0, /* No token in NVS; ready to initiate RFC 8628 flow */
    MUSE_AUTH_REQUESTING_CODE   = 1, /* Requesting device_code & user_code from cloud */
    MUSE_AUTH_WAIT_USER_VERIFY  = 2, /* Displaying QR + short code (e.g. ABC-123) and polling */
    MUSE_AUTH_AUTHORIZED        = 3, /* Token saved in NVS; connected to Muse AI */
    MUSE_AUTH_EXPIRED_OR_ERROR  = 4, /* Code expired or denied; retry on button press */
} muse_auth_state_t;

typedef enum {
    MUSE_VOICE_IDLE      = 0,
    MUSE_VOICE_LISTENING = 1,
    MUSE_VOICE_THINKING  = 2,
    MUSE_VOICE_SPEAKING  = 3,
} muse_voice_state_t;

typedef struct {
    muse_auth_state_t  auth_state;
    muse_voice_state_t voice_state;
    char               device_code[128];
    char               user_code[32];                 /* e.g. "ABC-123" */
    char               verification_uri[128];         /* e.g. "https://muse.ai/link" */
    char               verification_uri_complete[192];/* QR code target URL */
    uint32_t           expires_in_sec;
    uint32_t           poll_interval_sec;             /* RFC 8628 default 5s; +5s on slow_down */
    uint64_t           code_issued_ms;
    uint64_t           last_poll_ms;
    muse_credentials_t credentials;
    char               last_transcript[256];
    char               last_reply[256];
} muse_device_ctx_t;

/**
 * Initialize context and load existing long-lived credentials from `muse_cfg` NVS.
 * If already authorized in NVS, transitions directly to `MUSE_AUTH_AUTHORIZED`.
 */
void muse_device_init(muse_device_ctx_t *ctx);

/**
 * Begin RFC 8628 Device Authorization Grant when Wi-Fi is connected and no token is stored.
 * Formats the HTTP POST body (`client_id=...&scope=gadget:voice`) into `out_body`.
 */
int muse_oauth_build_device_code_req(const muse_device_ctx_t *ctx, char *out_body, size_t cap);

/**
 * Parse RFC 8628 §3.2 Device Authorization Response JSON:
 * {
 *   "device_code": "...",
 *   "user_code": "ABC-123",
 *   "verification_uri": "https://muse.ai/link",
 *   "verification_uri_complete": "https://muse.ai/link?code=ABC-123",
 *   "expires_in": 900,
 *   "interval": 5
 * }
 */
bool muse_oauth_on_device_code_resp(muse_device_ctx_t *ctx, const char *json, uint64_t now_ms);

/**
 * Check whether the background poller should send a token poll request at `now_ms`.
 * Automatically transitions to `MUSE_AUTH_EXPIRED_OR_ERROR` if `expires_in_sec` has elapsed.
 */
bool muse_oauth_should_poll(muse_device_ctx_t *ctx, uint64_t now_ms);

/**
 * Format RFC 8628 §3.4 Device Access Token Request body:
 * `grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=...&client_id=...`
 */
int muse_oauth_build_token_poll_req(const muse_device_ctx_t *ctx, char *out_body, size_t cap);

/**
 * Handle RFC 8628 §3.5 Device Access Token Response JSON:
 * - On `"access_token"`: saves credentials to NVS (`muse_cfg`) and enters `MUSE_AUTH_AUTHORIZED`
 * - On `"error":"authorization_pending"`: keeps waiting
 * - On `"error":"slow_down"`: increases `poll_interval_sec` by 5 seconds (RFC 8628 §3.5)
 * - On `"error":"expired_token"` or `"access_denied"`: enters `MUSE_AUTH_EXPIRED_OR_ERROR`
 */
bool muse_oauth_on_token_poll_resp(muse_device_ctx_t *ctx, const char *json, uint64_t now_ms);

/**
 * Clear stored Muse token from NVS (`muse_cfg`) and restart RFC 8628 Device Flow.
 */
bool muse_device_unpair(muse_device_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MUSE_OAUTH_DEVICE_H */
