#include "nevoflux_app.h"
#include <stdio.h>
#include <string.h>

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
