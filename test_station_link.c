// Full station lifecycle test. No OS networking, credential store, or USB I/O.
#include "station_link.h"
#include "radio/station_hw.h"
#include "security/cal_wpa.h"
#include "lease_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum ending { requested_stop, backend_read, backend_write, usb_lost, ap_left, ap_silent, backend_up };
struct run {
    enum ending ending;
    uint64_t now;
    int initial, frames, checks, starts, stops, keys, outbound, inbound;
    struct cal_wpa_ops wpa;
    uint8_t ap[6], sta[6];
} current;
volatile sig_atomic_t cal_cancelled;
const uint8_t cal_rsn_ie[22] = {0};
static const uint8_t network[] = {'l','a','b'};
static const uint8_t mock_key[16] = {1}, rsc[6] = {0};
static const uint8_t ordinary_src[6] = {2,1,2,3,4,5};

uint64_t cal_millis(void) { return current.now; }
int cal_write(void *ctx, unsigned q, const uint8_t *bytes, size_t size) {
    (void)ctx; (void)q; (void)bytes; (void)size;
    assert(!"unexpected USB write"); return -1;
}
int cal_read(struct pipes *p, struct incoming *rx) {
    (void)p;
    memset(rx,0,sizeof(*rx));
    rx->mpdu = rx->buffer; rx->size = 100;
    static const uint8_t script[] = {0xb0, 0x10, 0x08};
    assert(current.initial < 3);
    rx->buffer[0] = script[current.initial++];
    memcpy(rx->buffer+4,current.sta,6);
    memcpy(rx->buffer+10,current.ap,6);
    return 1;
}
int cal_read_for(struct pipes *p, struct incoming *rx, unsigned idle, unsigned total) {
    (void)p;
    assert(idle <= 25 && total <= 30 && current.starts == 1);
    current.frames++;
    memset(rx,0,sizeof(*rx));
    rx->mpdu = rx->buffer; rx->size = 100;
    if (current.ending == usb_lost) return -1;
    if (current.ending == ap_silent) {
        // Foreign beacons cannot keep a dead target alive.
        current.now += 2000; rx->buffer[0] = 0x80;
        memset(rx->buffer+4,255,6); return 1;
    }
    if (current.ending == ap_left) {
        rx->buffer[0] = 0xc0; rx->buffer[24] = 8;
        memset(rx->buffer+4,255,6);
        memcpy(rx->buffer+10,current.ap,6);
        memcpy(rx->buffer+16,current.ap,6);
        return 1;
    }
    // First packet is a protected GTK rekey. Then enough ordinary protected
    // traffic to keep the link alive well beyond the idle/handshake deadlines.
    current.now += 4000;
    rx->buffer[0] = 8; rx->buffer[1] = 0x42;
    return 1;
}

size_t calnet_secret(bool saved, const uint8_t *name, size_t n, uint8_t out[65]) {
    assert(saved && n == sizeof(network) && !memcmp(name,network,n));
    memset(out,'x',8); return 8;
}
int rtl_station_prepare_named(rtl_radio *r,const uint8_t ap[6],unsigned ch,unsigned rate,
    rtl_station_write_bulk cb,void *ctx,uint8_t mac[6],const uint8_t *ssid,size_t n) {
    (void)r; (void)cb; (void)ctx;
    assert(ch == 6 && rate == 2 && n == sizeof(network) && !memcmp(ssid, network, n));
    memcpy(current.ap,ap,6); memcpy(mac,current.sta,6); return 0;
}
const char *rtl_radio_error(const rtl_radio *r) { (void)r; return "simulated"; }
int rtl_radio_snapshot(rtl_radio *r,char *s,size_t n) { (void)r;(void)s;(void)n; return -1; }
int rtl_station_auth(rtl_radio *r,const uint8_t *p,size_t n) { (void)r;(void)p;(void)n;return 0; }
int rtl_station_assoc(rtl_radio *r,const uint8_t *p,size_t n) { return rtl_station_auth(r,p,n); }
int rtl_station_link(rtl_radio *r,unsigned aid,unsigned interval) { (void)r; assert(aid && interval);return 0; }
int rtl_station_keys(rtl_radio *r,const uint8_t *tk,unsigned id,const uint8_t *gtk,uint64_t seq) {
    (void)r;(void)tk;(void)gtk;(void)seq; assert(id == 1 || id == 2); ++current.keys;return 0;
}
size_t cal_auth_request(struct cal_station *s,uint8_t *f,size_t cap) { (void)s;(void)cap; f[0]=0xb0;return 30; }
size_t cal_assoc_request(struct cal_station *s,uint8_t *f,size_t cap) { return cal_auth_request(s,f,cap); }
size_t cal_eapol_data(struct cal_station *s,const uint8_t *p,size_t n,uint8_t *out,size_t cap) {
    (void)s;(void)p;(void)n;(void)out;(void)cap;return 32;
}
int rtl_station_eapol(rtl_radio *r,const uint8_t *p,size_t n) {return rtl_station_auth(r,p,n);}
int rtl_station_ethernet(rtl_radio *r,const uint8_t *p,size_t n) {
    (void)r; assert(n >= 14 && !memcmp(p+6,current.sta,6)); ++current.outbound; return 0;
}
int rtl_station_receive_ethernet(rtl_radio *r,uint32_t dw,const uint8_t *p,size_t n,
                                 uint8_t *eth,size_t cap,size_t *len) {
    (void)r; (void)dw; (void)p; (void)n; assert(cap >= 40);
    memset(eth,0,40); memcpy(eth,current.sta,6);
    if (current.frames == 1) {
        memcpy(eth+6,current.ap,6); eth[12]=0x88;eth[13]=0x8e;
    } else {
        memcpy(eth+6,ordinary_src,6); eth[12]=8;
    }
    *len=40; return 1;
}
enum cal_event cal_station_receive(struct cal_station *s,const uint8_t *p,size_t n,
                                   unsigned ch,struct cal_payload *msg) {
    (void)s;(void)n;(void)ch;
    if (p[0] == 0xb0) return CAL_AUTHENTICATED;
    if (p[0] == 0x10) return CAL_ASSOCIATED;
    if (p[0] == 8 && p[1] == 0) { msg->bytes=p;msg->length=16;return CAL_EAPOL; }
    return CAL_NONE;
}
cal_wpa *cal_wpa_start_named(const struct cal_wpa_ops *ops,const uint8_t sta[6],const uint8_t ap[6],
    const uint8_t *ours,size_t ours_n,const uint8_t *theirs,size_t theirs_n,
    const uint8_t *secret,size_t n,const uint8_t *ssid,size_t ssid_n) {
    (void)ours;(void)ours_n;(void)theirs;(void)theirs_n;(void)sta;(void)ap;
    assert(n == 8 && secret[0] == 'x' && ssid_n == 3 && !memcmp(ssid,network,3));
    current.wpa=*ops; return (cal_wpa *)&current;
}
int cal_wpa_receive(cal_wpa *w,const uint8_t ap[6],const uint8_t *p,size_t n,int encrypted) {
    (void)w;(void)ap;(void)p;(void)n;
    struct cal_wpa_ops *ops=&current.wpa;
    if (!encrypted) assert(ops->install_ccmp(ops->context,1,0,mock_key,rsc)==0);
    assert(ops->install_ccmp(ops->context,0,encrypted?2:1,mock_key,rsc)==0);
    return 1;
}
void cal_wpa_stop(cal_wpa *w) { assert(w); }
size_t dhcp_discover(const uint8_t m[6],uint32_t x,uint8_t *o,size_t n) {
    (void)m;(void)x;(void)o;(void)n;assert(!"persistent connection must use host DHCP");return 0;
}
bool dhcp_is_offer(const uint8_t *p,size_t n,const uint8_t m[6],uint32_t x) {
    (void)p;(void)n;(void)m;(void)x;assert(!"unexpected DHCP probe");return false;
}

static int up(void *ctx,const uint8_t mac[6]) {
    (void)ctx; assert(current.keys == 1 && !memcmp(mac,current.sta,6)); ++current.starts;
    return current.ending == backend_up ? -1 : 0;
}
static void down(void *ctx) { (void)ctx; ++current.stops; }
static int local_read(void *ctx,uint8_t *frame,size_t cap,size_t *len) {
    (void)ctx; assert(cap >= 60);
    if (current.ending == backend_read) return -1;
    if (current.checks++ == 0) {
        memset(frame,0,60); memcpy(frame+6,current.sta,6);frame[12]=8;*len=60; return 1;
    }
    return 0;
}
static int local_write(void *ctx,const uint8_t *p,size_t n) {
    (void)ctx; assert(n==40 && p[12]==8); ++current.inbound;
    return current.ending == backend_write ? -1 : 0;
}
static bool stop(void *ctx) { (void)ctx;return current.ending == requested_stop && current.frames == 8; }

int main(void) {
    for (enum ending e=requested_stop; e <= backend_up; ++e) {
        memset(&current,0,sizeof(current)); current.ending=e;current.sta[0]=2;
        struct pipes p={0};
        struct cal_station s={.ssid={'l','a','b'},.ssid_len=3,.channel=6,
            .bssid={2,0,0,0,0,1},.state=CAL_SELECTED,.rates={0x82},.rate_count=1,.aid=1,.beacon_interval=100};
        const struct cal_link_ops ops={NULL,up,local_read,local_write,down,stop};
        bool ok = cal_join_persistent((rtl_radio *)&current,&p,&s,true,&ops);
        assert(ok == (e==requested_stop));
        assert(current.starts==1 && current.stops==(e==backend_up?0:1));
        if (ok) assert(current.keys==2 && current.outbound==1 && current.inbound==7);
    }
    puts("PASS persistent station, rekey, forwarding, liveness and teardown");
}
