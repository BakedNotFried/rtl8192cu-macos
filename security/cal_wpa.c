#include "cal_wpa.h"
#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "rsn_supp/wpa.h"
#include <CommonCrypto/CommonKeyDerivation.h>

/* Single-thread, single-association lifetime. The parent bounds the association
 * timeout and clears device keys on every exit. Upstream owns all handshake state. */
struct cal_wpa {
    struct wpa_sm *sm;
    struct cal_wpa_ops ops;
    u8 ap[6];
    u8 ssid[SSID_MAX_LEN];
    size_t ssid_len;
    enum wpa_states state;
    bool failed, pairwise, group;
};
static bool busy;
static const u8 calnet[] = "CalNet";

static void state_set(void *ctx, enum wpa_states state) {
    ((cal_wpa *)ctx)->state = state;
}
static enum wpa_states state_get(void *ctx) { return ((cal_wpa *)ctx)->state; }
static void disconnected(void *ctx, u16 reason) {
    (void)reason;
    ((cal_wpa *)ctx)->failed = true;
}
static void reconnect(void *ctx) { ((cal_wpa *)ctx)->failed = true; }
static void *network_ctx(void *ctx) { return ctx; }
static int bssid_get(void *ctx, u8 *out) {
    memcpy(out, ((cal_wpa *)ctx)->ap, 6);
    return 0;
}
static int beacon_ie_ready(void *ctx) { (void)ctx; return 0; }
static void cancel_timeout(void *ctx) { (void)ctx; }
static int protection(void *ctx, const u8 *addr, int type, int key_type) {
    (void)ctx; (void)addr; (void)type; (void)key_type;
    return 0; /* Host data port stays closed until both install callbacks succeed. */
}
static u8 *allocate_frame(void *ctx, u8 type, const void *data, u16 length,
                          size_t *size, void **payload) {
    (void)ctx;
    if (type != IEEE802_1X_TYPE_EAPOL_KEY || length > 2300) return NULL;
    *size = sizeof(struct ieee802_1x_hdr) + length;
    struct ieee802_1x_hdr *hdr = os_zalloc(*size);
    if (!hdr) return NULL;
    hdr->version = 2;
    hdr->type = type;
    hdr->length = host_to_be16(length);
    if (data) memcpy(hdr + 1, data, length);
    if (payload) *payload = hdr + 1;
    return (u8 *)hdr;
}
static int ether_send(void *ctx, const u8 *dest, u16 proto, const u8 *buf, size_t len) {
    cal_wpa *w = ctx;
    if (w->failed || proto != ETH_P_EAPOL || memcmp(dest, w->ap, 6) ||
        w->ops.send_eapol(w->ops.context, buf, len)) {
        w->failed = true;
        return -1;
    }
    return 0;
}
static int set_key(void *ctx, int link, enum wpa_alg alg, const u8 *addr, int id,
                   int tx, const u8 *seq, size_t seq_len, const u8 *key,
                   size_t key_len, enum key_flag flags) {
    cal_wpa *w = ctx;
    /* The 8192CU does not stage a second receive-only pairwise key. Upstream
     * retries the normal pairwise install after transmitting message four. */
    if (flags == KEY_FLAG_PAIRWISE_NEXT) return -1;
    const bool pairwise = flags == KEY_FLAG_PAIRWISE_RX_TX;
    static const u8 broadcast[6] = {255,255,255,255,255,255};
    if (w->failed || link != -1 || alg != WPA_ALG_CCMP || !addr || !key || key_len != 16 ||
        !seq || seq_len != 6 || id < 0 || id > 3 ||
        (pairwise ? (id != 0 || tx != 1 || memcmp(addr, w->ap, 6)) :
                    (flags != KEY_FLAG_GROUP_RX || memcmp(addr, broadcast, 6))) ||
        w->ops.install_ccmp(w->ops.context, pairwise, (unsigned)id, key, seq)) {
        w->failed = true;
        return -1;
    }
    if (pairwise) w->pairwise = true;
    else w->group = true;
    return 0;
}

static bool rsn_ok(const u8 *ie, size_t length, bool station) {
    struct wpa_ie_data d;
    if (!ie || length < 2 || length > 257 || ie[0] != WLAN_EID_RSN ||
        (size_t)ie[1] + 2 != length || wpa_parse_wpa_ie_rsn(ie, length, &d))
        return false;
    if (d.proto != WPA_PROTO_RSN || d.group_cipher != WPA_CIPHER_CCMP ||
        (d.capabilities & WPA_CAPABILITY_MFPR)) return false;
    if (station)
        return d.pairwise_cipher == WPA_CIPHER_CCMP && d.key_mgmt == WPA_KEY_MGMT_PSK &&
               !(d.capabilities & WPA_CAPABILITY_MFPC);
    return (d.pairwise_cipher & WPA_CIPHER_CCMP) && (d.key_mgmt & WPA_KEY_MGMT_PSK);
}

static int derive_pmk(const u8 *secret, size_t len, const u8 *ssid, size_t ssid_len, u8 pmk[32]) {
    if (!secret || len < 8 || len > 64) return -1;
    if (len == 64) return hexstr2bin((const char *)secret, pmk, 32);
    for (size_t i = 0; i < len; ++i) {
        if (secret[i] < 32 || secret[i] > 126) return -1;
    }
    return CCKeyDerivationPBKDF(kCCPBKDF2, (const char *)secret, len, ssid, ssid_len,
                               kCCPRFHmacAlgSHA1, 4096, pmk, 32);
}

cal_wpa *cal_wpa_start(const struct cal_wpa_ops *ops, const u8 sta[6], const u8 ap[6],
                       const u8 *assoc, size_t assoc_len, const u8 *beacon, size_t beacon_len,
                       const u8 *secret, size_t secret_len) {
    return cal_wpa_start_named(ops, sta, ap, assoc, assoc_len, beacon, beacon_len, secret,
                               secret_len, calnet, sizeof(calnet)-1);
}

cal_wpa *cal_wpa_start_named(const struct cal_wpa_ops *ops, const u8 sta[6], const u8 ap[6],
                             const u8 *assoc, size_t assoc_len, const u8 *beacon, size_t beacon_len,
                             const u8 *secret, size_t secret_len, const u8 *ssid, size_t ssid_len) {
    if (busy || !ssid || !ssid_len || ssid_len > SSID_MAX_LEN ||
        !ops || !ops->send_eapol || !ops->install_ccmp || !sta || !ap ||
        is_zero_ether_addr(sta) || is_zero_ether_addr(ap) || (sta[0] & 1) || (ap[0] & 1) ||
        !memcmp(sta, ap, 6) || !rsn_ok(assoc, assoc_len, true) ||
        !rsn_ok(beacon, beacon_len, false)) return NULL;
    cal_wpa *w = os_zalloc(sizeof(*w));
    struct wpa_sm_ctx *ctx = os_zalloc(sizeof(*ctx));
    if (!w || !ctx || eloop_init()) { free(w); free(ctx); return NULL; }
    busy = true;
    w->ops = *ops;
    memcpy(w->ap, ap, 6);
    memcpy(w->ssid, ssid, ssid_len);
    w->ssid_len = ssid_len;
    ctx->ctx = w;
    ctx->set_state = state_set;
    ctx->get_state = state_get;
    ctx->deauthenticate = disconnected;
    ctx->reconnect = reconnect;
    ctx->set_key = set_key;
    ctx->get_network_ctx = network_ctx;
    ctx->get_bssid = bssid_get;
    ctx->ether_send = ether_send;
    ctx->get_beacon_ie = beacon_ie_ready;
    ctx->cancel_auth_timeout = cancel_timeout;
    ctx->alloc_eapol = allocate_frame;
    ctx->mlme_setprotection = protection;
    w->sm = wpa_sm_init(ctx);
    if (!w->sm) { free(ctx); cal_wpa_stop(w); return NULL; }
    struct rsn_supp_config profile = {
        .network_ctx = w, .allowed_pairwise_cipher = WPA_CIPHER_CCMP,
        .ssid = w->ssid, .ssid_len = w->ssid_len
    };
    wpa_sm_set_config(w->sm, &profile);
    wpa_sm_set_own_addr(w->sm, sta);
    wpa_sm_set_param(w->sm, WPA_PARAM_PROTO, WPA_PROTO_RSN);
    wpa_sm_set_param(w->sm, WPA_PARAM_PAIRWISE, WPA_CIPHER_CCMP);
    wpa_sm_set_param(w->sm, WPA_PARAM_GROUP, WPA_CIPHER_CCMP);
    wpa_sm_set_param(w->sm, WPA_PARAM_KEY_MGMT, WPA_KEY_MGMT_PSK);
    wpa_sm_set_param(w->sm, WPA_PARAM_RSN_ENABLED, 1);
    if (wpa_sm_set_assoc_wpa_ie(w->sm, assoc, assoc_len) ||
        wpa_sm_set_ap_rsn_ie(w->sm, beacon, beacon_len)) {
        cal_wpa_stop(w);
        return NULL;
    }
    u8 pmk[32] = {0};
    int error = derive_pmk(secret, secret_len, w->ssid, w->ssid_len, pmk);
    if (!error) wpa_sm_set_pmk(w->sm, pmk, sizeof(pmk), NULL, NULL);
    forced_memzero(pmk, sizeof(pmk));
    if (error) { cal_wpa_stop(w); return NULL; }
    wpa_sm_notify_assoc(w->sm, ap);
    w->state = WPA_ASSOCIATED;
    return w;
}

int cal_wpa_receive(cal_wpa *w, const u8 src[6], const u8 *frame, size_t len, int encrypted) {
    if (!w || w->failed) return -1;
    if (src && !memcmp(src, w->ap, 6) && frame && len >= 5 && len <= 2304 &&
        frame[1] == IEEE802_1X_TYPE_EAPOL_KEY && frame[4] == EAPOL_KEY_TYPE_RSN &&
        (size_t)WPA_GET_BE16(frame + 2) + 4 == len) {
        wpa_sm_rx_eapol(w->sm, src, frame, len, encrypted ? FRAME_ENCRYPTED : FRAME_NOT_ENCRYPTED);
    }
    return w->failed ? -1 : w->state == WPA_COMPLETED && w->group && w->pairwise;
}

void cal_wpa_stop(cal_wpa *w) {
    if (!w) return;
    wpa_sm_deinit(w->sm);
    eloop_destroy();
    forced_memzero(w, sizeof(*w));
    free(w);
    busy = false;
}
