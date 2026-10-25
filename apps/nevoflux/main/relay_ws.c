#include "nevoflux_app.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "mbedtls/sha256.h"
#else
#include <openssl/sha.h>
#endif

int nf_relay_build_ws_url(const char *base_relay_url,
                          const char *channel_id,
                          bool is_media_channel,
                          const char *jwt,
                          char *out_url,
                          size_t out_cap)
{
    if (!base_relay_url || !channel_id || !jwt || !out_url || out_cap == 0U) {
        return -1;
    }
    if (channel_id[0] == '\0' || jwt[0] == '\0') {
        return -1;
    }

    /* Strip trailing slash from base_relay_url if present */
    size_t base_len = strlen(base_relay_url);
    while (base_len > 0U && base_relay_url[base_len - 1U] == '/') {
        base_len--;
    }

    /* Matches portal-relay/src/relay-logic.ts: `/?c=<channel_id>[-m]&t=<jwt>` */
    int n = snprintf(out_url,
                     out_cap,
                     "%.*s/?c=%s%s&t=%s",
                     (int)base_len,
                     base_relay_url,
                     channel_id,
                     is_media_channel ? "-m" : "",
                     jwt);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}

int nf_lan_build_ws_url(const char *ip, uint16_t port, const char *channel_id, char *out_url, size_t out_cap)
{
    if (!ip || !ip[0] || !channel_id || !channel_id[0] || !out_url || out_cap == 0U) {
        return -1;
    }
    /* Same dial as the relay (`?c=`), minus the token: nevoflux-agent remote/lan.rs */
    int n = snprintf(out_url, out_cap, "ws://%s:%u/?c=%s", ip, (unsigned)(port ? port : NF_LAN_DEFAULT_PORT),
                     channel_id);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}

void nf_lan_channel_tag(const char *channel_id, char out_tag[9])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    const char *id = channel_id ? channel_id : "";
#if defined(ESP_PLATFORM)
    (void)mbedtls_sha256((const unsigned char *)id, strlen(id), digest, 0);
#else
    (void)SHA256((const unsigned char *)id, strlen(id), digest);
#endif
    for (size_t i = 0; i < 4U; ++i) {
        out_tag[2U * i] = hex[digest[i] >> 4];
        out_tag[2U * i + 1U] = hex[digest[i] & 0xFU];
    }
    out_tag[8] = '\0';
}

bool nf_lan_txt_has_tag(const char *txt_ch, const char *tag)
{
    if (!txt_ch || !tag || strlen(tag) != 8U) {
        return false;
    }
    const char *p = txt_ch;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 8U && memcmp(p, tag, 8U) == 0) {
            return true;
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return false;
}

int nf_wire_wrap_frame(const char *frame_json, char *out, size_t out_cap)
{
    if (!frame_json || !out || out_cap == 0U) {
        return -1;
    }
    /* Uplink frames carry no `seq`: the head ignores a sequenced one as another head's */
    int n = snprintf(out, out_cap, "{\"k\":\"frame\",\"frame\":%s}", frame_json);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}
