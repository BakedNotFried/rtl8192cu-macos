#include "lease_probe.h"
#include <string.h>

static unsigned be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static unsigned checksum(const uint8_t *p, size_t n) {
    unsigned sum = 0;
    for (size_t i = 0; i < n; i += 2) sum += be16(p + i);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (~sum) & 0xffff;
}
static const uint8_t cookie[] = {99, 130, 83, 99};
static const uint8_t broadcast[] = {255, 255, 255, 255, 255, 255};

size_t dhcp_discover(const uint8_t mac[6], uint32_t xid, uint8_t *out, size_t cap) {
    enum { bootp_bytes = 300, ip_bytes = 28 + bootp_bytes, total = 14 + ip_bytes };
    if (cap < total) return 0;
    memset(out, 0, total);
    memcpy(out, broadcast, 6);
    memcpy(out + 6, mac, 6);
    put16(out + 12, 0x0800);
    uint8_t *ip = out + 14, *udp = ip + 20, *b = udp + 8;
    ip[0] = 0x45;
    put16(ip + 2, ip_bytes);
    ip[8] = 64;
    ip[9] = 17;
    memset(ip + 16, 255, 4);
    put16(ip + 10, checksum(ip, 20));
    put16(udp, 68); put16(udp + 2, 67); put16(udp + 4, bootp_bytes + 8);
    b[0] = b[1] = 1; b[2] = 6;
    put32(b + 4, xid);
    memcpy(b + 28, mac, 6);
    memcpy(b + 236, cookie, sizeof(cookie));
    const uint8_t options[] = {53, 1, 1, 255};
    memcpy(b + 240, options, sizeof(options));
    return total;
}

bool dhcp_is_offer(const uint8_t *f, size_t n, const uint8_t mac[6], uint32_t xid) {
    if (n < 14 + 20 + 8 + 240 || be16(f + 12) != 0x0800 ||
        (memcmp(f, mac, 6) && memcmp(f, broadcast, 6))) return false;
    const uint8_t *ip = f + 14;
    unsigned ihl = (ip[0] & 15u) * 4u, length = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || length > n - 14 || length < ihl + 8 + 240 ||
        ip[9] != 17 || (be16(ip + 6) & 0x3fff) || checksum(ip, ihl)) return false;
    const uint8_t *udp = ip + ihl, *b = udp + 8;
    unsigned ulen = be16(udp + 4);
    uint8_t wanted[4]; put32(wanted, xid);
    if (be16(udp) != 67 || be16(udp + 2) != 68 || ulen < 248 || ulen > length - ihl ||
        b[0] != 2 || b[1] != 1 || b[2] != 6 || memcmp(b + 4, wanted, 4) ||
        memcmp(b + 28, mac, 6) || memcmp(b + 236, cookie, 4)) return false;
    bool offer = false;
    for (unsigned i = 240; i < ulen - 8;) {
        unsigned opt = b[i++];
        if (!opt) continue;
        if (opt == 255) return offer;
        if (i >= ulen - 8) break;
        unsigned size = b[i++];
        if (size > ulen - 8 - i) break;
        if (opt == 53) {
            if (offer || size != 1 || b[i] != 2) return false;
            offer = true;
        }
        i += size;
    }
    return false;
}
