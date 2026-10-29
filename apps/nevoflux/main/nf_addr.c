#include "nf_addr.h"

#include <string.h>

bool nf_addr_url_host(const char *url, char *host, size_t cap)
{
    if (!url || !host || cap == 0) {
        return false;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?#");
    if (n == 0 || n >= cap) {
        return false;
    }
    memcpy(host, p, n);
    host[n] = '\0';
    return true;
}

size_t nf_addr_dns_query(const char *name, uint16_t id, uint8_t *buf, size_t cap)
{
    if (!name || !buf || cap < 12) {
        return 0;
    }
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)id;
    buf[2] = 0x01; /* recursion desired */
    buf[5] = 1;    /* one question */
    size_t off = 12;
    const char *label = name;
    while (*label) {
        size_t n = strcspn(label, ".");
        if (n == 0 || n > 63 || off + 1 + n >= cap) {
            return 0;
        }
        buf[off++] = (uint8_t)n;
        memcpy(buf + off, label, n);
        off += n;
        label += n;
        if (*label == '.') {
            ++label;
        }
    }
    if (off + 5 > cap) {
        return 0;
    }
    buf[off++] = 0;
    buf[off++] = 0; /* type A */
    buf[off++] = 1;
    buf[off++] = 0; /* class IN */
    buf[off++] = 1;
    return off;
}

/* Offset just past the name at off, 0 if it runs off the end */
static size_t skip_name(const uint8_t *m, size_t len, size_t off)
{
    while (off < len) {
        uint8_t c = m[off];
        if (c == 0) {
            return off + 1;
        }
        if ((c & 0xC0) == 0xC0) {
            return off + 2 <= len ? off + 2 : 0;
        }
        if (c & 0xC0) {
            return 0;
        }
        off += 1U + c;
    }
    return 0;
}

int nf_addr_dns_parse(const uint8_t *msg, size_t len, uint16_t id, uint32_t *ips, int max)
{
    if (!msg || len < 12 || ((uint16_t)(msg[0] << 8 | msg[1])) != id || !(msg[2] & 0x80)) {
        return -1;
    }
    if ((msg[3] & 0x0F) != 0) {
        return 0; /* NXDOMAIN, SERVFAIL, ...: no addresses */
    }
    unsigned qd = (unsigned)(msg[4] << 8 | msg[5]);
    unsigned an = (unsigned)(msg[6] << 8 | msg[7]);
    size_t off = 12;
    for (unsigned i = 0; i < qd; ++i) {
        off = skip_name(msg, len, off);
        if (!off || off + 4 > len) {
            return -1;
        }
        off += 4;
    }
    int count = 0;
    for (unsigned i = 0; i < an; ++i) {
        off = skip_name(msg, len, off);
        if (!off || off + 10 > len) {
            return -1;
        }
        unsigned type = (unsigned)(msg[off] << 8 | msg[off + 1]);
        unsigned cls = (unsigned)(msg[off + 2] << 8 | msg[off + 3]);
        size_t rdlen = (size_t)(msg[off + 8] << 8 | msg[off + 9]);
        off += 10;
        if (off + rdlen > len) {
            return -1;
        }
        if (type == 1 && cls == 1 && rdlen == 4 && count < max) { /* CNAMEs are skipped */
            memcpy(&ips[count++], msg + off, 4);
        }
        off += rdlen;
    }
    return count;
}

int nf_addr_order(const uint32_t *list, int n, uint32_t preferred, uint32_t failed, int *out)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (list[i] == preferred && preferred != failed) {
            out[k++] = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (list[i] != failed && !(list[i] == preferred && preferred != failed)) {
            out[k++] = i;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (list[i] == failed) {
            out[k++] = i;
        }
    }
    return k;
}
