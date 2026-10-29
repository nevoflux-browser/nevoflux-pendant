#pragma once
/*
 * Pins the address lwIP hands out for a host (getaddrinfo, so the WebSocket and
 * HTTP clients alike) to one that answers a TCP connect right now. The name stays
 * the same everywhere, so TLS still checks the certificate against it and the
 * Host header is unchanged. Called from the link task only.
 */
#include <stdbool.h>
#include <stdint.h>

/* Look up all of host's addresses and pin the first that connects: the one that
 * worked last time first, the one that just failed last. Nothing connects: unpin,
 * and the system resolver answers as before. */
void nf_resolve_pick(const char *host);

/* The pinned address of host failed us; the next pick tries it last */
void nf_resolve_failed(const char *host);

/* TCP connect time to ip (network byte order) on port in ms; -1 timeout, -3 no socket, -4 refused/reset */
int nf_resolve_tcp_ms(uint32_t ip, uint16_t port, int timeout_ms);
