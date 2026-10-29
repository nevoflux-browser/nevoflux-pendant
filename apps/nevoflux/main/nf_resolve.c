#include "nf_resolve.h"
#include "nf_addr.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

static const char *TAG = "nf_resolve";

#define NF_RESOLVE_HOSTS    2    /* the account site and the relay */
#define NF_RESOLVE_TRY_MS   3000 /* a working path answers in ~200 ms */
#define NF_RESOLVE_DNS_MS   2000

typedef struct {
    char     host[64];
    uint32_t ips[NF_ADDR_MAX];
    int      n;
    uint32_t pinned; /* 0: let the system resolver answer */
    uint32_t failed;
} entry_t;

static entry_t      s_hosts[NF_RESOLVE_HOSTS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *ip_str(uint32_t ip, char out[16])
{
    struct in_addr a = {.s_addr = ip};
    return inet_ntoa_r(a, out, 16);
}

/* lwIP asks this before every name lookup (CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM) */
int lwip_hook_netconn_external_resolve(const char *name, ip_addr_t *addr, u8_t addrtype, err_t *err)
{
    if (!name || addrtype == NETCONN_DNS_IPV6) {
        return 0;
    }
    uint32_t ip = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < NF_RESOLVE_HOSTS; ++i) {
        if (s_hosts[i].pinned && strcasecmp(name, s_hosts[i].host) == 0) {
            ip = s_hosts[i].pinned;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!ip) {
        return 0;
    }
    ip_addr_set_ip4_u32(addr, ip);
    *err = ERR_OK;
    return 1;
}

static entry_t *entry_for(const char *host)
{
    entry_t *free_slot = NULL;
    for (int i = 0; i < NF_RESOLVE_HOSTS; ++i) {
        if (s_hosts[i].host[0] && strcasecmp(host, s_hosts[i].host) == 0) {
            return &s_hosts[i];
        }
        if (!s_hosts[i].host[0] && !free_slot) {
            free_slot = &s_hosts[i];
        }
    }
    if (!free_slot || strlen(host) >= sizeof(free_slot->host)) {
        return NULL;
    }
    taskENTER_CRITICAL(&s_mux);
    strcpy(free_slot->host, host);
    taskEXIT_CRITICAL(&s_mux);
    return free_slot;
}

/* Every A record, straight from the DNS server: lwIP's resolver keeps only the first */
static int dns_lookup_all(const char *host, uint32_t *ips, int max)
{
    const ip_addr_t *d = dns_getserver(0);
    if (!d || !IP_IS_V4(d) || ip4_addr_isany_val(*ip_2_ip4(d))) {
        return -1;
    }
    static uint8_t msg[512]; /* the link task only */
    uint16_t id = (uint16_t)esp_random();
    size_t qlen = nf_addr_dns_query(host, id, msg, sizeof(msg));
    if (!qlen) {
        return -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    struct timeval tv = {.tv_sec = NF_RESOLVE_DNS_MS / 1000, .tv_usec = 0};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(53)};
    sa.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(d));
    int k = -1;
    if (sendto(s, msg, qlen, 0, (struct sockaddr *)&sa, sizeof(sa)) == (int)qlen) {
        for (int tries = 0; tries < 3 && k < 0; ++tries) { /* skip a stray late reply */
            int r = recv(s, msg, sizeof(msg), 0);
            if (r <= 0) {
                break;
            }
            k = nf_addr_dns_parse(msg, (size_t)r, id, ips, max);
        }
    }
    close(s);
    return k;
}

int nf_resolve_tcp_ms(uint32_t ip, uint16_t port, int timeout_ms)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    sa.sin_addr.s_addr = ip;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -3;
    }
    (void)fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int64_t t0 = esp_timer_get_time();
    int rc = -1;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        rc = (int)((esp_timer_get_time() - t0) / 1000);
    } else if (errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        if (select(s + 1, NULL, &w, NULL, &tv) > 0) {
            int e = 0;
            socklen_t len = sizeof(e);
            (void)getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &len);
            rc = e == 0 ? (int)((esp_timer_get_time() - t0) / 1000) : -4;
        }
    } else {
        rc = -4;
    }
    close(s);
    return rc;
}

void nf_resolve_pick(const char *host)
{
    entry_t *e = host ? entry_for(host) : NULL;
    if (!e) {
        return;
    }
    uint32_t found[NF_ADDR_MAX];
    int k = dns_lookup_all(host, found, NF_ADDR_MAX);
    if (k > 0) {
        /* What DNS says now, plus the pinned one so it is not dropped mid-rotation */
        memcpy(e->ips, found, (size_t)k * sizeof(found[0]));
        e->n = k;
        bool have = false;
        for (int i = 0; i < k; ++i) {
            have |= e->ips[i] == e->pinned;
        }
        if (e->pinned && !have && e->n < NF_ADDR_MAX) {
            e->ips[e->n++] = e->pinned;
        }
    }
    int order[NF_ADDR_MAX];
    int m = nf_addr_order(e->ips, e->n, e->pinned, e->failed, order);
    uint32_t chosen = 0;
    char a[16], b[16];
    for (int i = 0; i < m && !chosen; ++i) {
        uint32_t ip = e->ips[order[i]];
        int ms = nf_resolve_tcp_ms(ip, 443, NF_RESOLVE_TRY_MS);
        if (ms >= 0) {
            chosen = ip;
        } else {
            ESP_LOGW(TAG, "%s: %s does not answer (%s)", host, ip_str(ip, a), ms == -1 ? "timeout" : "refused");
        }
    }
    if (chosen != e->pinned) {
        if (chosen) {
            ESP_LOGI(TAG, "%s: using %s%s%s", host, ip_str(chosen, a), e->pinned ? " instead of " : "",
                     e->pinned ? ip_str(e->pinned, b) : "");
        } else if (m) {
            ESP_LOGW(TAG, "%s: no address answers; leaving it to DNS", host);
        }
    }
    if (chosen == e->failed) {
        e->failed = 0;
    }
    taskENTER_CRITICAL(&s_mux);
    e->pinned = chosen;
    taskEXIT_CRITICAL(&s_mux);
}

void nf_resolve_failed(const char *host)
{
    entry_t *e = host ? entry_for(host) : NULL;
    if (e && e->pinned) {
        e->failed = e->pinned;
    }
}
