#include "muse_oauth_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    if (!json || !key || !out || cap == 0U) {
        return false;
    }
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return false;
    }
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p != ':') {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
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
        if (i + 1U >= cap) {
            return false;
        }
        out[i++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out[i] = '\0';
    return true;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out_val)
{
    if (!json || !key || !out_val) {
        return false;
    }
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return false;
    }
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p != ':') {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    *out_val = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

void muse_device_init(muse_device_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->poll_interval_sec = 5U;
    ctx->expires_in_sec = 900U;
    ctx->voice_state = MUSE_VOICE_IDLE;

    if (prov_nvs_load_muse_cfg(&ctx->credentials)) {
        ctx->auth_state = MUSE_AUTH_AUTHORIZED;
    } else {
        ctx->auth_state = MUSE_AUTH_UNPAIRED;
    }
}

int muse_oauth_build_device_code_req(const muse_device_ctx_t *ctx, char *out_body, size_t cap)
{
    (void)ctx;
    if (!out_body || cap == 0U) {
        return -1;
    }
    return snprintf(out_body, cap,
                    "client_id=%s&scope=gadget%%3Avoice+gadget%%3Adisplay",
                    MUSE_OAUTH_CLIENT_ID);
}

bool muse_oauth_on_device_code_resp(muse_device_ctx_t *ctx, const char *json, uint64_t now_ms)
{
    if (!ctx || !json) {
        return false;
    }
    if (!json_get_str(json, "device_code", ctx->device_code, sizeof(ctx->device_code)) ||
        ctx->device_code[0] == '\0' ||
        !json_get_str(json, "user_code", ctx->user_code, sizeof(ctx->user_code)) ||
        ctx->user_code[0] == '\0') {
        ctx->auth_state = MUSE_AUTH_EXPIRED_OR_ERROR;
        return false;
    }

    if (!json_get_str(json, "verification_uri", ctx->verification_uri, sizeof(ctx->verification_uri)) ||
        ctx->verification_uri[0] == '\0') {
        snprintf(ctx->verification_uri, sizeof(ctx->verification_uri), "%s", MUSE_OAUTH_DEFAULT_VERIFY);
    }

    if (!json_get_str(json, "verification_uri_complete",
                      ctx->verification_uri_complete,
                      sizeof(ctx->verification_uri_complete)) ||
        ctx->verification_uri_complete[0] == '\0') {
        snprintf(ctx->verification_uri_complete,
                 sizeof(ctx->verification_uri_complete),
                 "%s?code=%s",
                 ctx->verification_uri,
                 ctx->user_code);
    }

    uint32_t exp = 900U;
    uint32_t interval = 5U;
    (void)json_get_u32(json, "expires_in", &exp);
    (void)json_get_u32(json, "interval", &interval);

    ctx->expires_in_sec = (exp > 0U) ? exp : 900U;
    ctx->poll_interval_sec = (interval > 0U) ? interval : 5U;
    ctx->code_issued_ms = now_ms;
    ctx->last_poll_ms = now_ms;
    ctx->auth_state = MUSE_AUTH_WAIT_USER_VERIFY;
    return true;
}

bool muse_oauth_should_poll(muse_device_ctx_t *ctx, uint64_t now_ms)
{
    if (!ctx || ctx->auth_state != MUSE_AUTH_WAIT_USER_VERIFY) {
        return false;
    }
    if (now_ms >= ctx->code_issued_ms &&
        (now_ms - ctx->code_issued_ms) >= (uint64_t)ctx->expires_in_sec * 1000ULL) {
        ctx->auth_state = MUSE_AUTH_EXPIRED_OR_ERROR;
        return false;
    }
    if (now_ms >= ctx->last_poll_ms &&
        (now_ms - ctx->last_poll_ms) >= (uint64_t)ctx->poll_interval_sec * 1000ULL) {
        ctx->last_poll_ms = now_ms;
        return true;
    }
    return false;
}

int muse_oauth_build_token_poll_req(const muse_device_ctx_t *ctx, char *out_body, size_t cap)
{
    if (!ctx || !out_body || cap == 0U || ctx->device_code[0] == '\0') {
        return -1;
    }
    return snprintf(out_body, cap,
                    "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code"
                    "&device_code=%s&client_id=%s",
                    ctx->device_code,
                    MUSE_OAUTH_CLIENT_ID);
}

bool muse_oauth_on_token_poll_resp(muse_device_ctx_t *ctx, const char *json, uint64_t now_ms)
{
    (void)now_ms;
    if (!ctx || !json) {
        return false;
    }

    char access_token[sizeof(ctx->credentials.access_token)];
    if (json_get_str(json, "access_token", access_token, sizeof(access_token)) &&
        access_token[0] != '\0') {
        memset(&ctx->credentials, 0, sizeof(ctx->credentials));
        snprintf(ctx->credentials.access_token, sizeof(ctx->credentials.access_token), "%s", access_token);
        (void)json_get_str(json, "refresh_token", ctx->credentials.refresh_token, sizeof(ctx->credentials.refresh_token));
        if (!json_get_str(json, "device_id", ctx->credentials.device_id, sizeof(ctx->credentials.device_id))) {
            snprintf(ctx->credentials.device_id, sizeof(ctx->credentials.device_id), "MuseGadget-143C");
        }
        ctx->credentials.is_authorized = true;

        if (!prov_nvs_save_muse_cfg(&ctx->credentials)) {
            return false;
        }
        ctx->auth_state = MUSE_AUTH_AUTHORIZED;
        return true;
    }

    char err[64];
    if (json_get_str(json, "error", err, sizeof(err))) {
        if (strcmp(err, "authorization_pending") == 0) {
            return false;
        }
        if (strcmp(err, "slow_down") == 0) {
            /* RFC 8628 Section 3.5: increase polling interval by 5 seconds */
            ctx->poll_interval_sec += 5U;
            return false;
        }
        if (strcmp(err, "expired_token") == 0 || strcmp(err, "access_denied") == 0) {
            ctx->auth_state = MUSE_AUTH_EXPIRED_OR_ERROR;
            return false;
        }
    }
    return false;
}

bool muse_device_unpair(muse_device_ctx_t *ctx)
{
    if (!ctx) {
        return false;
    }
    (void)prov_nvs_clear_muse_cfg();
    memset(&ctx->credentials, 0, sizeof(ctx->credentials));
    ctx->device_code[0] = '\0';
    ctx->user_code[0] = '\0';
    ctx->auth_state = MUSE_AUTH_UNPAIRED;
    ctx->voice_state = MUSE_VOICE_IDLE;
    return true;
}
