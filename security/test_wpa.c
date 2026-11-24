/* In-memory interoperability test. Only the built-in local peers participate. */
#include "cal_wpa.h"
#include "includes.h"
#include "common.h"
#include "ap/wpa_auth.h"
#include <CommonCrypto/CommonKeyDerivation.h>
#include <assert.h>

static const u8 station_addr[6] = {2,0,0,0,0,1}, ap_addr[6] = {2,0,0,0,0,2};
static const u8 rsn[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};

struct packet { u8 bytes[2304]; size_t len; };
struct fixture {
    struct packet to_ap, to_station;
    u8 pmk[32], station_keys[2][16], ap_keys[2][16];
    bool have_key[2], authorized;
};
static int put(struct packet *p, const u8 *bytes, size_t n) {
    assert(!p->len && n && n <= sizeof(p->bytes));
    memcpy(p->bytes, bytes, n);
    p->len = n;
    return 0;
}
static int from_station(void *ctx, const u8 *data, size_t n) {
    return put(&((struct fixture *)ctx)->to_ap, data, n);
}
static int from_ap(void *ctx, const u8 *dest, const u8 *data, size_t n, int encrypted) {
    assert(!memcmp(dest, station_addr, 6) && !encrypted);
    return put(&((struct fixture *)ctx)->to_station, data, n);
}
static int station_key(void *ctx, int pair, unsigned id, const u8 key[16], const u8 rsc[6]) {
    struct fixture *f = ctx;
    assert(id <= 3 && rsc);
    assert(!f->have_key[pair]);
    memcpy(f->station_keys[pair], key, 16);
    f->have_key[pair] = true;
    return 0;
}
static int ap_key(void *ctx, int vlan, enum wpa_alg alg, const u8 *addr, int idx,
                  u8 *key, size_t n, enum key_flag flags) {
    (void)vlan; (void)addr; (void)idx;
    if (alg == WPA_ALG_CCMP) {
        assert(n == 16);
        memcpy(((struct fixture *)ctx)->ap_keys[!!(flags & KEY_FLAG_PAIRWISE)], key, n);
    }
    return 0;
}
static const u8 *ap_psk(void *ctx, const u8 *addr, const u8 *p2p, const u8 *prev,
                        size_t *n, int *vlan_id) {
    (void)p2p;
    assert(!memcmp(addr, station_addr, 6));
    if (prev) return NULL;
    if (n) *n = 32;
    if (vlan_id) *vlan_id = 0;
    return ((struct fixture *)ctx)->pmk;
}
static void set_eapol(void *ctx, const u8 *addr, wpa_eapol_variable var, int val) {
    assert(!memcmp(addr, station_addr, 6));
    if (var == WPA_EAPOL_authorized)
        ((struct fixture *)ctx)->authorized = !!val;
}
static void test_exchange(const u8 *ssid, size_t ssid_len, bool named) {
    struct fixture f = {0};
    struct cal_wpa_ops ops = {&f, from_station, station_key};
    const char test_phrase[] = "unit-test fixture";
    assert(!CCKeyDerivationPBKDF(kCCPBKDF2, test_phrase, strlen(test_phrase),
        ssid, ssid_len, kCCPRFHmacAlgSHA1, 4096, f.pmk, 32));
    u8 transient_ssid[32];
    memcpy(transient_ssid, ssid, ssid_len);
    cal_wpa *sta;
    if (named)
        sta = cal_wpa_start_named(&ops, station_addr, ap_addr, rsn, sizeof(rsn), rsn, sizeof(rsn),
                                  (const u8 *)test_phrase, strlen(test_phrase), transient_ssid, ssid_len);
    else
        sta = cal_wpa_start(&ops, station_addr, ap_addr, rsn, sizeof(rsn), rsn, sizeof(rsn),
                            (const u8 *)test_phrase, strlen(test_phrase));
    memset(transient_ssid, 0xff, sizeof(transient_ssid));
    assert(sta);

    struct wpa_auth_config conf = {
        .wpa = WPA_PROTO_RSN, .wpa_key_mgmt = WPA_KEY_MGMT_PSK,
        .wpa_group = WPA_CIPHER_CCMP, .rsn_pairwise = WPA_CIPHER_CCMP,
        .wpa_pairwise_update_count = 4, .wpa_group_update_count = 4,
        .eapol_version = 2, .ssid_len = ssid_len
    };
    memcpy(conf.ssid, ssid, ssid_len);
    struct wpa_auth_callbacks cb = {
        .set_key = ap_key, .get_psk = ap_psk, .send_eapol = from_ap, .set_eapol = set_eapol
    };
    struct wpa_authenticator *ap = wpa_init(ap_addr, &conf, &cb, &f);
    assert(ap && !wpa_init_keys(ap));
    size_t beacon_len = 0;
    const u8 *ap_ie = wpa_auth_get_wpa_ie(ap, &beacon_len);
    assert(ap_ie && beacon_len >= sizeof(rsn) && !memcmp(ap_ie, rsn, sizeof(rsn)));
    struct wpa_state_machine *peer = wpa_auth_sta_init(ap, station_addr, NULL);
    assert(peer && wpa_validate_wpa_ie(ap, peer, 2412, rsn, sizeof(rsn),
                                      NULL, 0, NULL, 0, NULL, 0, NULL, false) == WPA_IE_OK);
    assert(!wpa_auth_sta_associated(ap, peer));
    for (int round = 0; round < 2; ++round) {
        assert(f.to_station.len);
        assert(cal_wpa_receive(sta, station_addr, f.to_station.bytes, f.to_station.len, 0) == 0);
        assert(!f.to_ap.len && !f.have_key[0] && !f.have_key[1]);
        int status = cal_wpa_receive(sta, ap_addr, f.to_station.bytes, f.to_station.len, 0);
        if (status != round)
            fprintf(stderr, "fixture round %d status %d, pairwise=%d group=%d outbound=%zu\n",
                    round, status, f.have_key[1], f.have_key[0], f.to_ap.len);
        assert(status == round);
        f.to_station.len = 0;
        assert(f.to_ap.len);
        wpa_receive(ap, peer, f.to_ap.bytes, f.to_ap.len);
        f.to_ap.len = 0;
    }
    assert(f.authorized && f.have_key[0] && f.have_key[1]);
    assert(!memcmp(f.station_keys, f.ap_keys, sizeof(f.station_keys)));
    wpa_auth_sta_deinit(peer);
    wpa_deinit(ap);
    cal_wpa_stop(sta);
    forced_memzero(&f, sizeof(f));
}

int main(void) {
    static const u8 fixture_ssid[] = {'f','i','x',0,'2'};
    test_exchange((const u8 *)"CalNet", 6, false);
    test_exchange(fixture_ssid, sizeof(fixture_ssid), true);
    puts("Both hostap in-memory handshakes completed; CCMP pairwise and group keys matched");
}
