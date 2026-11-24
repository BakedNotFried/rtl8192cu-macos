#include "calnet_io.h"
#include "lease_probe.h"
#include "radio/station_hw.h"
#include "security/cal_wpa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t calnet_secret(bool saved, const uint8_t *name, size_t length, uint8_t out[65]);
struct connection {
    rtl_radio *radio;
    struct pipes *usb;
    struct cal_station *station;
    uint8_t tk[16], gtk[16];
    unsigned gtk_id;
    uint64_t rsc;
    bool have_tk, have_gtk, installed;
};
static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}
static bool hardware_ok(rtl_radio *r, int rc, const char *what) {
    if (!rc) return true;
    fprintf(stderr,"%s: %s\n",what,rtl_radio_error(r));
    return false;
}
static bool management(struct connection *c, bool auth) {
    const char *name = auth ? "Open System authentication" : "Association";
    for (unsigned attempt = 0; attempt < 3 && !cal_cancelled; ++attempt) {
        uint8_t tx[256];
        size_t n = auth ? cal_auth_request(c->station,tx,sizeof(tx)) :
                          cal_assoc_request(c->station,tx,sizeof(tx));
        if (!n) return false;
        int rc = auth ? rtl_station_auth(c->radio,tx,n) : rtl_station_assoc(c->radio,tx,n);
        if (!hardware_ok(c->radio,rc,name)) return false;
        uint64_t until = cal_millis() + 1800;
        while (!cal_cancelled && cal_millis() < until) {
            struct incoming rx;
            int result = cal_read(c->usb,&rx);
            if (result < 0) return false;
            if (!result) continue;
            enum cal_event event = cal_station_receive(c->station,rx.mpdu,rx.size,
                                                        c->station->channel,NULL);
            if (event == CAL_REJECTED || event == CAL_DISCONNECTED) {
                fprintf(stderr,"%s declined by selected AP, status/reason %u\n",name,c->station->last_status);
                return false;
            }
            if (event == (auth ? CAL_AUTHENTICATED : CAL_ASSOCIATED)) {
                printf("%s accepted by selected AP\n",name);
                return true;
            }
        }
    }
    fprintf(stderr,"%s: no matching AP response within the retry window\n",name);
    return false;
}

static int send_key_message(void *context, const uint8_t *msg, size_t len) {
    struct connection *c = context;
    uint8_t frame[2336];
    int rc;
    if (c->installed) {
        if (len > 1500) return -1;
        memcpy(frame,c->station->bssid,6);
        memcpy(frame+6,c->station->mac,6);
        frame[12]=0x88; frame[13]=0x8e;
        memcpy(frame+14,msg,len);
        rc = rtl_station_ethernet(c->radio,frame,len+14);
    } else {
        size_t n = cal_eapol_data(c->station,msg,len,frame,sizeof(frame));
        if (!n) return -1;
        rc = rtl_station_eapol(c->radio,frame,n);
    }
    wipe(frame,sizeof(frame));
    return hardware_ok(c->radio,rc,"Send EAPOL response") ? 0 : -1;
}
static int install_key(void *context, int pair, unsigned id, const uint8_t key[16], const uint8_t seq[6]) {
    struct connection *c = context;
    if (pair) {
        memcpy(c->tk,key,16); c->have_tk = true;
    } else {
        memcpy(c->gtk,key,16); c->have_gtk = true; c->gtk_id=id;
        c->rsc = 0;
        for (int i=5; i>=0; --i) c->rsc = (c->rsc << 8) | seq[i];
    }
    if (!c->have_tk || !c->have_gtk) return 0;
    int rc = rtl_station_keys(c->radio,c->tk,c->gtk_id,c->gtk,c->rsc);
    if (!hardware_ok(c->radio,rc,"Install station keys")) return -1;
    c->installed = true;
    return 0;
}

static bool exchange(struct connection *c, cal_wpa *wpa) {
    uint64_t deadline = cal_millis()+14000, next_discover = 0;
    uint32_t xid = arc4random();
    bool authenticated = false;
    unsigned attempts = 0, key_messages = 0;
    while (!cal_cancelled && cal_millis() < deadline) {
        if (authenticated && attempts < 3 && cal_millis() >= next_discover) {
            uint8_t discover[400];
            size_t n = dhcp_discover(c->station->mac,xid,discover,sizeof(discover));
            if (!n || !hardware_ok(c->radio,rtl_station_ethernet(c->radio,discover,n),
                                   "Send encrypted DHCP discover")) return false;
            next_discover = cal_millis()+2200;
            ++attempts;
        }
        struct incoming rx;
        int res = cal_read(c->usb,&rx);
        if (res < 0) return false;
        if (!res) continue;
        struct cal_payload msg = {0};
        enum cal_event event = cal_station_receive(c->station,rx.mpdu,rx.size,c->station->channel,&msg);
        if (event == CAL_DISCONNECTED) {
            fprintf(stderr,"AP ended the association (reason %u)\n", c->station->last_status);
            return false;
        }
        uint8_t ethernet[1514];
        bool protected_msg = false;
        if (c->installed && (rx.mpdu[1] & 0x40)) {
            size_t bytes = 0;
            int accepted = rtl_station_receive_ethernet(c->radio,rx.descriptor,rx.mpdu,rx.size,
                                                        ethernet,sizeof(ethernet),&bytes);
            if (accepted < 0) return false;
            if (!accepted) continue;
            if (authenticated && attempts && dhcp_is_offer(ethernet,bytes,c->station->mac,xid)) {
                puts("Encrypted bidirectional data verified by a matching DHCP offer");
                return true;
            }
            if (bytes < 14 || memcmp(ethernet+6,c->station->bssid,6) ||
                ethernet[12]!=0x88 || ethernet[13]!=0x8e) continue;
            msg = (struct cal_payload){ethernet+14,bytes-14};
            protected_msg = true;
        } else if (event != CAL_EAPOL) continue;
        ++key_messages;
        int state = cal_wpa_receive(wpa,c->station->bssid,msg.bytes,msg.length,protected_msg);
        if (state < 0) { fprintf(stderr,"Supplicant ended the key exchange\n"); return false; }
        if (state == 1 && !authenticated) {
            if (!c->installed) return false;
            puts("WPA2 four-way handshake complete; CCMP keys installed");
            authenticated = true;
            deadline = cal_millis()+8500;
        }
    }
    fprintf(stderr,"%s timed out (%u EAPOL messages received)\n",
            authenticated ? "Encrypted DHCP proof" : "WPA2 handshake",key_messages);
    return false;
}

bool cal_join(rtl_radio *r, struct pipes *p, struct cal_station *s, const char *mode) {
    unsigned rate = 255;
    for (unsigned i=0; i<s->rate_count; ++i) {
        unsigned v = s->rates[i]&127u;
        if ((s->rates[i]&128) && v<rate) rate=v;
    }
    if (rate == 255) { fprintf(stderr,"AP has no supported basic rate\n"); return false; }
    struct connection c = {.radio=r,.usb=p,.station=s};
    if (!hardware_ok(r,rtl_station_prepare_named(r,s->bssid,s->channel,rate,cal_write,p,s->mac,
                                                  s->ssid,s->ssid_len),
                     "Prepare station")) return false;
    const bool association_only = !strcmp(mode,"--associate");
    uint8_t secret[65] = {0};
    size_t n = 0;
    if (!association_only) {
        n = calnet_secret(!strcmp(mode,"--join-saved"),s->ssid,s->ssid_len,secret);
        if (!n) {
            wipe(secret,sizeof(secret));
            fprintf(stderr,"Selected network credential unavailable\n");
            return false;
        }
    }
    bool ok = management(&c,true) && management(&c,false) &&
              hardware_ok(r,rtl_station_link(r,s->aid,s->beacon_interval),"Enable station link");
    cal_wpa *wpa = NULL;
    if (ok && !association_only) {
        struct cal_wpa_ops ops = {&c,send_key_message,install_key};
        wpa = cal_wpa_start_named(&ops,s->mac,s->bssid,cal_rsn_ie,sizeof(cal_rsn_ie),
                                  s->ap_rsn,s->ap_rsn_len,secret,n,s->ssid,s->ssid_len);
        if (!wpa) { fprintf(stderr,"Supplicant initialization failed\n"); ok=false; }
    }
    wipe(secret,sizeof(secret));
    if (ok && wpa) ok = exchange(&c,wpa);
    cal_wpa_stop(wpa);
    wipe(&c,sizeof(c));
    return ok;
}
