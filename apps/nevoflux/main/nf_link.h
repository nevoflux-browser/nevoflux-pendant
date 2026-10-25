#ifndef NF_LINK_H
#define NF_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "prov_nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The pendant's way to its head (nevoflux-agent daemon), ESP only.
 *
 * LAN first: browse mDNS `_nevoflux._tcp` for a head whose TXT `ch` lists this
 * pairing's data channel tag, and dial it straight (`ws://ip:port/?c=`).
 * Otherwise the relay (`wss://relay/?c=&t=<15-min JWT>`), re-probing the LAN
 * every couple of minutes so walking in the door moves the pendant off it.
 *
 * Two sockets, same as the phone portal: control (sync -> sessions -> attach
 * the most recently updated conversation) and data (speech up, answers down).
 * Every binary frame is `WireMessage` JSON sealed with that channel's key.
 */

typedef enum {
    NF_LINK_DOWN  = 0,
    NF_LINK_LAN   = 1,
    NF_LINK_RELAY = 2,
} nf_link_mode_t;

typedef struct {
    nf_link_mode_t mode;
    bool           data_up;       /* data socket connected */
    bool           head_online;   /* the head is on the channel (peers > 0, or LAN) */
    bool           attached;      /* a conversation is bound to the data channel */
    bool           auth_revoked;  /* account token refused: re-pair */
    char           session_title[64];
} nf_link_status_t;

/** Start the link task. `cred` is copied. */
bool nf_link_start(const nf_credentials_t *cred);

/** Replace credentials (after re-provisioning) and reconnect. */
void nf_link_restart(const nf_credentials_t *cred);

/** Seal and send one frame object on the data channel. False if not connected. */
bool nf_link_send_frame(const char *frame_json);

/**
 * Next decrypted data-channel plaintext (`WireMessage` JSON, NUL-terminated),
 * or NULL. The caller frees it with nf_link_free().
 */
char *nf_link_poll(void);
void nf_link_free(char *msg);

void nf_link_get_status(nf_link_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NF_LINK_H */
