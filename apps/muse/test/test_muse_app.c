#include "muse_oauth_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(prov_nvs_init());

    /* 1. Fresh factory boot: no token in NVS -> MUSE_AUTH_UNPAIRED */
    muse_device_ctx_t ctx;
    muse_device_init(&ctx);
    assert(ctx.auth_state == MUSE_AUTH_UNPAIRED);

    /* 2. Build RFC 8628 Device Authorization Request */
    char req_body[256];
    assert(muse_oauth_build_device_code_req(&ctx, req_body, sizeof(req_body)) > 0);
    assert(strstr(req_body, "client_id=muse-gadget-esp32-s3-143c") != NULL);

    /* 3. Receive RFC 8628 §3.2 response with device_code, user_code ("ABC-123"), verification_uri */
    const char *code_resp =
        "{"
        "\"device_code\":\"dev_code_998877665544\","
        "\"user_code\":\"ABC-123\","
        "\"verification_uri\":\"https://muse.ai/link\","
        "\"verification_uri_complete\":\"https://muse.ai/link?code=ABC-123\","
        "\"expires_in\":600,"
        "\"interval\":5"
        "}";
    assert(muse_oauth_on_device_code_resp(&ctx, code_resp, 1000ULL));
    assert(ctx.auth_state == MUSE_AUTH_WAIT_USER_VERIFY);
    assert(strcmp(ctx.user_code, "ABC-123") == 0);
    assert(strcmp(ctx.verification_uri, "https://muse.ai/link") == 0);
    assert(strcmp(ctx.verification_uri_complete, "https://muse.ai/link?code=ABC-123") == 0);
    assert(ctx.poll_interval_sec == 5U);
    assert(ctx.expires_in_sec == 600U);

    /* 4. Polling schedule: should not poll before 5s interval */
    assert(!muse_oauth_should_poll(&ctx, 4000ULL));
    assert(muse_oauth_should_poll(&ctx, 6000ULL));

    char poll_req[256];
    assert(muse_oauth_build_token_poll_req(&ctx, poll_req, sizeof(poll_req)) > 0);
    assert(strstr(poll_req, "device_code=dev_code_998877665544") != NULL);

    /* 5. Poll response: authorization_pending -> stays in WAIT_USER_VERIFY */
    assert(!muse_oauth_on_token_poll_resp(&ctx, "{\"error\":\"authorization_pending\"}", 6100ULL));
    assert(ctx.auth_state == MUSE_AUTH_WAIT_USER_VERIFY);
    assert(ctx.poll_interval_sec == 5U);

    /* 6. Poll response: slow_down -> RFC 8628 §3.5 adds +5s to poll_interval_sec (5 -> 10s) */
    assert(!muse_oauth_on_token_poll_resp(&ctx, "{\"error\":\"slow_down\"}", 11100ULL));
    assert(ctx.auth_state == MUSE_AUTH_WAIT_USER_VERIFY);
    assert(ctx.poll_interval_sec == 10U);
    assert(!muse_oauth_should_poll(&ctx, 14000ULL)); /* only 8s since last poll at 6000ms */
    assert(muse_oauth_should_poll(&ctx, 16000ULL));  /* 10s elapsed */

    /* 7. User authorizes in browser -> cloud returns long-lived access_token -> saved to NVS */
    const char *tok_resp =
        "{"
        "\"access_token\":\"mgst_live_oauth2_device_flow_token_7788\","
        "\"refresh_token\":\"mgrt_live_refresh_9900\","
        "\"device_id\":\"MuseGadget-143C-8899\","
        "\"token_type\":\"Bearer\""
        "}";
    assert(muse_oauth_on_token_poll_resp(&ctx, tok_resp, 16200ULL));
    assert(ctx.auth_state == MUSE_AUTH_AUTHORIZED);

    /* 8. Reboot simulation: muse_device_init loads token directly from NVS */
    muse_device_ctx_t rebooted_ctx;
    muse_device_init(&rebooted_ctx);
    assert(rebooted_ctx.auth_state == MUSE_AUTH_AUTHORIZED);
    assert(strcmp(rebooted_ctx.credentials.access_token, "mgst_live_oauth2_device_flow_token_7788") == 0);
    assert(strcmp(rebooted_ctx.credentials.device_id, "MuseGadget-143C-8899") == 0);

    /* 9. Unpair clears NVS and returns to UNPAIRED */
    assert(muse_device_unpair(&rebooted_ctx));
    assert(rebooted_ctx.auth_state == MUSE_AUTH_UNPAIRED);

    printf("[PASS] All Muse (ota_0) RFC 8628 Device Flow & SDK tests passed!\n");
    return 0;
}
