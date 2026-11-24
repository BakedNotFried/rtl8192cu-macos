#include "station.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const uint8_t default_ssid[] = {'C','a','l','N','e','t'};
static const uint8_t broadcast[6] = {255,255,255,255,255,255};
static const uint8_t llc_eapol[8] = {0xaa,0xaa,3,0,0,0,0x88,0x8e};
static const uint8_t ccmp[4] = {0,15,172,4}, psk[4] = {0,15,172,2};
const uint8_t cal_rsn_ie[22] = {
    48,20, 1,0, 0,15,172,4, 1,0, 0,15,172,4, 1,0, 0,15,172,2, 0,0
};

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void set16(uint8_t *p, uint16_t value) { p[0] = value & 255; p[1] = value >> 8; }
static bool unicast(const uint8_t *addr)
{
    static const uint8_t zero[6];
    return !(addr[0] & 1) && memcmp(addr, zero, 6) != 0;
}

void cal_station_init(struct cal_station *s, const uint8_t mac[6])
{
    (void)cal_station_init_ssid(s,mac,default_ssid,sizeof(default_ssid));
}

bool cal_station_init_ssid(struct cal_station *s, const uint8_t mac[6],
                           const uint8_t *ssid, size_t ssid_len)
{
    if (!s) return false;
    struct cal_station fresh = {0};
    if (mac && unicast(mac) && ssid && ssid_len && ssid_len <= sizeof(fresh.ssid)) {
        memcpy(fresh.mac, mac, 6);
        memcpy(fresh.ssid,ssid,ssid_len);
        fresh.ssid_len = (uint8_t)ssid_len;
    } else {
        fresh.state = CAL_FAILED;
    }
    *s = fresh;
    return fresh.state == CAL_SCANNING;
}

/* Consume one counted suite list, requiring the negotiated suite be present. */
static bool offers(const uint8_t **p, size_t *left, const uint8_t suite[4])
{
    if (*left < 2) return false;
    size_t n = le16(*p);
    *p += 2; *left -= 2;
    if (!n || n > *left / 4) return false;
    bool found = false;
    for (size_t i = 0; i < n; i++, *p += 4)
        if (memcmp(*p, suite, 4) == 0) found = true;
    *left -= 4 * n;
    return found;
}

static bool compatible_rsn(const uint8_t *p, size_t n)
{
    if (n < 6 || le16(p) != 1 || memcmp(p + 2, ccmp, 4)) return false;
    p += 6; n -= 6;
    if (!offers(&p, &n, ccmp) || !offers(&p, &n, psk)) return false;
    if (!n) return true;
    if (n < 2 || (le16(p) & (1u << 6))) return false; /* PMF required */
    p += 2; n -= 2;
    if (!n) return true;
    if (n < 2) return false;
    size_t pmkids = le16(p);
    n -= 2;
    if (pmkids > n / 16) return false;
    n -= 16 * pmkids;
    return n == 0 || n == 4; /* optional group management cipher */
}

static bool add_rates(struct cal_station *s, const uint8_t *p, size_t n)
{
    static const uint8_t legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    for (size_t i = 0; i < n; i++) {
        uint8_t rate = p[i] & 127;
        if (!memchr(legacy, rate, sizeof(legacy))) {
            if (p[i] & 128) return false;
            continue;
        }
        unsigned j = 0;
        while (j < s->rate_count && (s->rates[j] & 127) != rate) j++;
        if (j == s->rate_count) s->rate_count++;
        s->rates[j] |= p[i];
    }
    return true;
}

static const char *suite_name(const uint8_t *s, bool akm)
{
    if (s[0] != 0 || s[1] != 15 || s[2] != 172) return "vendor";
    if (akm) {
        switch (s[3]) {
        case 1: return "802.1X";
        case 2: return "PSK";
        case 3: return "FT-802.1X";
        case 4: return "FT-PSK";
        case 5: return "802.1X-SHA256";
        case 6: return "PSK-SHA256";
        case 8: return "SAE";
        case 9: return "FT-SAE";
        case 18: return "OWE";
        default: return "other";
        }
    }
    switch (s[3]) {
    case 1: return "WEP40";
    case 2: return "TKIP";
    case 4: return "CCMP";
    case 5: return "WEP104";
    case 8: return "GCMP";
    case 9: return "GCMP-256";
    case 10: return "CCMP-256";
    default: return "other";
    }
}

static bool suite_list(const uint8_t **field, size_t *remaining, bool akm,
                       char *names, size_t capacity)
{
    if (*remaining < 2) return false;
    unsigned count = le16(*field);
    *field += 2;
    *remaining -= 2;
    if (!count || count > *remaining / 4) return false;
    size_t used = 0;
    for (unsigned i = 0; i < count; i++, *field += 4) {
        if (used < capacity - 1) {
            int k = snprintf(names + used, capacity - used, "%s%s",
                             i ? "," : "", suite_name(*field, akm));
            if (k > 0) used += (size_t)k;
        }
    }
    *remaining -= count * 4;
    return true;
}

bool cal_beacon_diagnostic(const uint8_t *f, size_t n, unsigned listening,
                           unsigned *advertised, char *message, size_t capacity)
{
    return cal_named_beacon_diagnostic(f,n,listening,advertised,message,capacity,
                                      default_ssid,sizeof(default_ssid));
}

bool cal_named_beacon_diagnostic(const uint8_t *f, size_t n, unsigned listening,
                                 unsigned *advertised, char *message, size_t capacity,
                                 const uint8_t *ssid, size_t ssid_len)
{
    if (!message || !capacity || !advertised) return false;
    message[0] = '\0';
    *advertised = 0;
    if (!f || n < 36 || f[0] != 0x80 || !ssid || !ssid_len || ssid_len > 32)
        return false;
    unsigned ds = 0, ht = 0, ssids = 0, rsns = 0, base_rates = 0;
    bool named = false, rate_ok = true, rsn_ok = false;
    struct cal_station rates = {0};
    const char *group = "absent", *pmf = "unknown";
    char pair[96] = "absent", akm[96] = "absent";
    size_t tail = 0;
    for (size_t i = 36; i < n;) {
        size_t left = n - i;
        if (left < 2 || left - 2 < f[i+1]) {
            tail = left;
            break;
        }
        uint8_t id = f[i], len = f[i+1];
        const uint8_t *p = f + i + 2;
        i += 2u + len;
        switch (id) {
        case 0:
            ssids++;
            if (len == ssid_len && !memcmp(p, ssid, len)) named = true;
            break;
        case 3:
            ds = len == 1 ? p[0] : 255;
            break;
        case 61:
            ht = len >= 1 ? p[0] : 255;
            break;
        case 1:
        case 50:
            if (id == 1 && (++base_rates != 1 || !len || len > 8)) rate_ok = false;
            if (!add_rates(&rates, p, len)) rate_ok = false;
            break;
        case 48: {
            rsns++;
            rsn_ok = compatible_rsn(p, len);
            if (len < 6 || le16(p) != 1) break;
            group = suite_name(p+2, false);
            p += 6;
            size_t size = len - 6;
            if (!suite_list(&p,&size,false,pair,sizeof(pair)) ||
                !suite_list(&p,&size,true,akm,sizeof(akm))) break;
            if (size != 1) pmf = size >= 2 && (le16(p) & 64) ? "yes" : "no";
            break;
        }
        }
    }
    if (!named) return false;
    unsigned ch = ds ? ds : ht;
    if (ch >= 1 && ch <= 13) *advertised = ch;
    int len = snprintf(message,capacity,
        "tuned=%u ds=%u ht=%u infrastructure=%u privacy=%u standard-header=%u "
        "ssid-count=%u basic-rates=%s supported-rates=%u rsn-count=%u group=%s "
        "pairwise=%s akm=%s pmf-required=%s rsn-eligible=%u trailing-invalid-bytes=%zu",
        listening, ds, ht, (le16(f+34) & 3) == 1, !!(f[34] & 16),
        !(f[1] & 0xc7) && !(f[22] & 15) && !memcmp(f+4,broadcast,6) &&
        unicast(f+16) && !memcmp(f+10,f+16,6) && le16(f+32) != 0,
        ssids, rate_ok && base_rates == 1 ? "ok" : "unsupported-or-malformed",
        (unsigned)rates.rate_count, rsns, group, pair, akm, pmf, rsn_ok, tail);
    return len >= 0 && (size_t)len < capacity;
}

static enum cal_event discover(struct cal_station *s, const uint8_t *f, size_t n,
                               unsigned channel)
{
    if (n < 36 || f[0] != 0x80 || (f[1] & 0xc7) || (f[22] & 15) ||
        memcmp(f + 4, broadcast, 6) || !unicast(f + 16) || memcmp(f + 10, f + 16, 6) ||
        !memcmp(f + 16,s->mac,6) || !le16(f + 32)) return CAL_NONE;
    uint16_t cap = le16(f + 34);
    if ((cap & 0x13) != 0x11) return CAL_NONE; /* infrastructure and privacy */
    struct cal_station next = *s;
    bool named = false, rates = false;
    for (size_t i = 36; i < n;) {
        if (n - i < 2) return CAL_NONE;
        uint8_t id = f[i], len = f[i+1];
        i += 2;
        if (len > n - i) return CAL_NONE;
        const uint8_t *p = f + i;
        switch (id) {
        case 0:
            if (named || !s->ssid_len || len != s->ssid_len || memcmp(p, s->ssid, len))
                return CAL_NONE;
            named = true;
            break;
        case 1:
            if (rates || !len || len > 8) return CAL_NONE;
            rates = true;
            if (!add_rates(&next, p, len)) return CAL_NONE;
            break;
        case 50:
            if (!add_rates(&next, p, len)) return CAL_NONE;
            break;
        case 3:
            if (len != 1 || p[0] != channel) return CAL_NONE;
            break;
        case 61:
            if (len < 1 || p[0] != channel) return CAL_NONE;
            break;
        case 48:
            if (next.ap_rsn_len || !compatible_rsn(p, len)) return CAL_NONE;
            next.ap_rsn_len = (size_t)len + 2;
            memcpy(next.ap_rsn, p - 2, next.ap_rsn_len);
            break;
        }
        i += len;
    }
    if (!named || !rates || !next.rate_count || !next.ap_rsn_len) return CAL_NONE;
    next.channel = (uint8_t)channel;
    next.beacon_interval = le16(f + 32);
    memcpy(next.bssid, f + 16, 6);
    next.capability = cap & 0x0431; /* ESS, privacy, short preamble, short slot */
    next.state = CAL_SELECTED;
    *s = next;
    return CAL_FOUND;
}

static bool key_packet(const uint8_t *p, size_t n)
{
    /* Accept only RSN EAPOL-Key with a complete, internally sized body. */
    return n >= 99 && n <= 2304 - sizeof(llc_eapol) && p[0] >= 1 && p[0] <= 3 &&
           p[1] == 3 && n == 4u + ((unsigned)p[2] << 8 | p[3]) && p[4] == 2 &&
           n == 99u + ((unsigned)p[97] << 8 | p[98]);
}

enum cal_event cal_station_receive(struct cal_station *s, const uint8_t *f, size_t n,
                                   unsigned channel, struct cal_payload *eapol)
{
    if (eapol) *eapol = (struct cal_payload){0};
    if (!s || !f || n < 24 || channel < 1 || channel > 13) return CAL_NONE;
    if (s->state == CAL_SCANNING) return discover(s,f,n,channel);
    if (s->state == CAL_FAILED || channel != s->channel || memcmp(f + 4,s->mac,6) ||
        memcmp(f + 10,s->bssid,6) || memcmp(f + 16,s->bssid,6) ||
        (f[1] & 0xc4) || (f[22] & 15)) return CAL_NONE;

    if ((f[1] & 3) == 0) {
        if (s->state == CAL_WAIT_AUTH && f[0] == 0xb0 && n >= 30 &&
            le16(f + 24) == 0 && le16(f + 26) == 2) {
            s->last_status = le16(f + 28);
            s->state = s->last_status ? CAL_FAILED : CAL_AUTH_OK;
            return s->last_status ? CAL_REJECTED : CAL_AUTHENTICATED;
        }
        if (s->state == CAL_WAIT_ASSOC && f[0] == 0x10 && n >= 30) {
            s->last_status = le16(f + 26);
            if (s->last_status) { s->state = CAL_FAILED; return CAL_REJECTED; }
            uint16_t aid = le16(f + 28) & 0x3fff;
            if (!aid || aid > 2007) return CAL_NONE;
            s->aid = aid;
            s->state = CAL_ASSOC_OK;
            return CAL_ASSOCIATED;
        }
        if ((f[0] == 0xc0 || f[0] == 0xa0) && n >= 26 && s->state >= CAL_WAIT_AUTH) {
            s->last_status = le16(f + 24);
            s->state = CAL_FAILED;
            return CAL_DISCONNECTED;
        }
        return CAL_NONE;
    }
    if (s->state != CAL_ASSOC_OK || (f[1] & 3) != 2 ||
        (f[0] != 0x08 && f[0] != 0x88)) return CAL_NONE;
    size_t header = f[0] == 0x88 ? 26 : 24;
    if (n < header + sizeof(llc_eapol) || (header == 26 && (f[24] & 0x80)) ||
        memcmp(f + header,llc_eapol,sizeof(llc_eapol))) return CAL_NONE;
    size_t offset = header + sizeof(llc_eapol);
    if (!key_packet(f + offset, n - offset)) return CAL_NONE;
    if (eapol) *eapol = (struct cal_payload){f + offset, n - offset};
    return CAL_EAPOL;
}

/* Each builder creates one protocol-specific frame for this selected peer. */
static void header(struct cal_station *s, uint8_t *out, uint8_t type, uint8_t flags)
{
    memset(out,0,24);
    out[0] = type; out[1] = flags;
    memcpy(out + 4,s->bssid,6); memcpy(out + 10,s->mac,6); memcpy(out + 16,s->bssid,6);
    set16(out + 22, (uint16_t)(s->sequence << 4));
    s->sequence = (s->sequence + 1) & 0xfff;
}

size_t cal_auth_request(struct cal_station *s, uint8_t *out, size_t cap)
{
    if (!s || !out || cap < 30 || (s->state != CAL_SELECTED && s->state != CAL_WAIT_AUTH))
        return 0;
    header(s,out,0xb0,0);
    set16(out + 24,0); set16(out + 26,1); set16(out + 28,0);
    s->state = CAL_WAIT_AUTH;
    return 30;
}

size_t cal_assoc_request(struct cal_station *s, uint8_t *out, size_t cap)
{
    if (!s || !out || (s->state != CAL_AUTH_OK && s->state != CAL_WAIT_ASSOC) ||
        !s->rate_count || s->rate_count > sizeof(s->rates)) return 0;
    if (!s->ssid_len || s->ssid_len > sizeof(s->ssid)) return 0;
    size_t n = 28 + 2 + s->ssid_len + 2 + s->rate_count +
               (s->rate_count > 8 ? 2 : 0) + sizeof(cal_rsn_ie);
    if (cap < n) return 0;
    header(s,out,0,0); set16(out + 24,s->capability); set16(out + 26,1);
    uint8_t *p = out + 28;
    *p++ = 0;
    *p++ = s->ssid_len;
    memcpy(p,s->ssid,s->ssid_len);
    p += s->ssid_len;
    unsigned basic = s->rate_count > 8 ? 8 : s->rate_count;
    *p++ = 1; *p++ = (uint8_t)basic; memcpy(p,s->rates,basic); p += basic;
    if (basic < s->rate_count) {
        unsigned extra = s->rate_count - basic;
        *p++ = 50; *p++ = (uint8_t)extra; memcpy(p,s->rates + basic,extra); p += extra;
    }
    memcpy(p,cal_rsn_ie,sizeof(cal_rsn_ie));
    s->state = CAL_WAIT_ASSOC;
    return n;
}

size_t cal_eapol_data(struct cal_station *s, const uint8_t *eapol, size_t n,
                      uint8_t *out, size_t cap)
{
    if (!s || s->state != CAL_ASSOC_OK || !out || !eapol || !key_packet(eapol,n) ||
        cap < 32 + n) return 0;
    header(s,out,8,1); memcpy(out + 24,llc_eapol,sizeof(llc_eapol));
    memcpy(out + 32,eapol,n);
    return 32 + n;
}
