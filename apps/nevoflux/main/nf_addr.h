#pragma once
/*
 * Which of a host's addresses to connect to. A name on Cloudflare has two or
 * more addresses that all serve it, and DNS rotates their order; a hotspot's
 * path to one of them can go dead for minutes while the other answers at once.
 * The pure parts (DNS wire format, try order) live here so they are tested on
 * the host; nf_resolve.c does the sockets and pins the choice into lwIP.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NF_ADDR_MAX 4

/* Host part of a URL ("wss://relay.x.app:443/p" -> "relay.x.app"); false if none or too long */
bool nf_addr_url_host(const char *url, char *host, size_t cap);

/* A DNS query for name's A records into buf; returns its length, 0 if it does not fit */
size_t nf_addr_dns_query(const char *name, uint16_t id, uint8_t *buf, size_t cap);

/* The A records (network byte order) in the reply to query id: how many (<= max), -1 if malformed or not that reply */
int nf_addr_dns_parse(const uint8_t *msg, size_t len, uint16_t id, uint32_t *ips, int max);

/* Indices of list in the order to try: preferred first (unless it is the one that just
 * failed), then the others as listed, the failed one last. Returns how many. */
int nf_addr_order(const uint32_t *list, int n, uint32_t preferred, uint32_t failed, int *out);
