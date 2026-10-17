#include "nevoflux_app.h"
#include <stdio.h>
#include <string.h>

void nf_token_mgr_init(nf_token_mgr_t *mgr)
{
    if (!mgr) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

bool nf_token_mgr_needs_refresh(const nf_token_mgr_t *mgr, uint64_t now_ms)
{
    if (!mgr || mgr->auth_revoked) {
        return false;
    }
    if (!mgr->valid || mgr->jwt[0] == '\0') {
        return true;
    }
    uint64_t elapsed = (now_ms >= mgr->minted_at_ms) ? (now_ms - mgr->minted_at_ms) : 0ULL;
    return elapsed >= (NF_JWT_TTL_MS - NF_JWT_REFRESH_MARGIN_MS);
}

int nf_token_mgr_build_request_headers(const char *account_token, char *out_hdrs, size_t out_cap)
{
    if (!account_token || account_token[0] == '\0' || !out_hdrs || out_cap == 0U) {
        return -1;
    }
    /* Cloudflare 1010 requires non-empty custom User-Agent */
    int n = snprintf(out_hdrs,
                     out_cap,
                     "Authorization: Bearer %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: application/json\r\n",
                     account_token,
                     NF_HTTP_USER_AGENT);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}

bool nf_token_mgr_parse_response(nf_token_mgr_t *mgr,
                                 int http_status,
                                 const char *json_body,
                                 uint64_t now_ms)
{
    if (!mgr) {
        return false;
    }
    if (http_status == 401 || http_status == 403) {
        mgr->valid = false;
        mgr->auth_revoked = true;
        mgr->jwt[0] = '\0';
        return false;
    }
    if (http_status != 200 || !json_body) {
        return false;
    }

    const char *key = strstr(json_body, "\"token\"");
    if (!key) {
        return false;
    }
    const char *colon = strchr(key + 7, ':');
    if (!colon) {
        return false;
    }
    const char *q1 = strchr(colon + 1, '"');
    if (!q1) {
        return false;
    }
    ++q1;
    const char *q2 = strchr(q1, '"');
    if (!q2 || q2 == q1) {
        return false;
    }

    size_t tok_len = (size_t)(q2 - q1);
    if (tok_len >= sizeof(mgr->jwt)) {
        return false;
    }
    memcpy(mgr->jwt, q1, tok_len);
    mgr->jwt[tok_len] = '\0';
    mgr->minted_at_ms = now_ms;
    mgr->valid = true;
    mgr->auth_revoked = false;
    return true;
}
