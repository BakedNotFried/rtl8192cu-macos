#include "station.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t client[6] = {2,0,0,0,0,1};
static const uint8_t ap[6] = {2,0,0,0,0,2};
static const uint8_t beacon_base[] = {
    0x80,0, 0,0, 255,255,255,255,255,255, 2,0,0,0,0,2, 2,0,0,0,0,2, 0,0,
    0,0,0,0,0,0,0,0, 100,0, 0x31,4, 0,6,'C','a','l','N','e','t',
    1,8,0x82,0x84,0x8b,0x96,12,18,24,36, 3,1,6, 50,4,48,72,96,108
};

static size_t build_beacon(uint8_t *out)
{
    memcpy(out,beacon_base,sizeof(beacon_base));
    memcpy(out + sizeof(beacon_base),cal_rsn_ie,sizeof(cal_rsn_ie));
    return sizeof(beacon_base) + sizeof(cal_rsn_ie);
}

static void reply(uint8_t *out, uint8_t subtype, uint8_t flags)
{
    memset(out,0,256);
    out[0] = subtype;
    out[1] = flags;
    memcpy(out + 4,client,6);
    memcpy(out + 10,ap,6);
    memcpy(out + 16,ap,6);
}

static void check_named_stations(void)
{
    struct cal_station default_station, selected;
    uint8_t frame[256], association[256];
    char report[640];
    unsigned advertised;
    const uint8_t network[] = {'T','e','s','t',0,'A','P'};
    size_t n = build_beacon(frame);
    cal_station_init(&default_station,client);
    assert(!cal_station_init_ssid(&selected,client,network,0));
    assert(!cal_station_init_ssid(&selected,client,network,33));
    assert(!cal_station_init_ssid(&selected,client,NULL,1));
    assert(cal_station_init_ssid(&selected,client,network,sizeof(network)));
    assert(cal_station_receive(&selected,frame,n,6,NULL) == CAL_NONE);
    const size_t default_tail = 38 + 6;
    memmove(frame + 38 + sizeof(network),frame + default_tail,n - default_tail);
    memcpy(frame + 38,network,sizeof(network));
    frame[37] = sizeof(network);
    n += sizeof(network) - 6;
    assert(!cal_beacon_diagnostic(frame,n,6,&advertised,report,sizeof(report)));
    assert(cal_named_beacon_diagnostic(frame,n,6,&advertised,report,sizeof(report),
                                      network,sizeof(network)));
    assert(cal_station_receive(&default_station,frame,n,6,NULL) == CAL_NONE);
    assert(cal_station_receive(&selected,frame,n,6,NULL) == CAL_FOUND);
    assert(cal_auth_request(&selected,association,sizeof(association)) == 30);
    reply(frame,0xb0,0);
    frame[26] = 2;
    assert(cal_station_receive(&selected,frame,30,6,NULL) == CAL_AUTHENTICATED);
    assert(cal_assoc_request(&selected,association,sizeof(association)) == 75);
    assert(association[29] == sizeof(network) &&
           !memcmp(association + 30,network,sizeof(network)));
}

int main(void)
{
    check_named_stations();
    struct cal_station s;
    struct cal_payload payload;
    uint8_t f[256], out[320];
    cal_station_init(&s,client);
    size_t n = build_beacon(f);
    char report[640];
    unsigned advertised;
    assert(cal_beacon_diagnostic(f,n,1,&advertised,report,sizeof(report)) &&
           advertised == 6 && strstr(report,"pairwise=CCMP akm=PSK") &&
           strstr(report,"pmf-required=no") && strstr(report,"trailing-invalid-bytes=0"));
    assert(cal_beacon_diagnostic(f,n-1,1,&advertised,report,sizeof(report)) &&
           strstr(report,"trailing-invalid-bytes=21"));
    assert(!cal_beacon_diagnostic(f,n,1,&advertised,report,2));
    assert(cal_auth_request(&s,out,sizeof(out)) == 0);
    assert(cal_station_receive(&s,f,n,11,NULL) == CAL_NONE); /* adjacent channel */
    assert(cal_station_receive(&s,f,n-1,6,NULL) == CAL_NONE);
    f[38] = 'X';
    assert(!cal_beacon_diagnostic(f,n,6,&advertised,report,sizeof(report)));
    assert(cal_station_receive(&s,f,n,6,NULL) == CAL_NONE);
    n = build_beacon(f);
    f[n - 2] |= 0x40; /* PMF required */
    assert(cal_beacon_diagnostic(f,n,6,&advertised,report,sizeof(report)) &&
           strstr(report,"pmf-required=yes rsn-eligible=0"));
    assert(cal_station_receive(&s,f,n,6,NULL) == CAL_NONE);
    n = build_beacon(f);
    f[n - 3] = 8; /* SAE only */
    assert(cal_station_receive(&s,f,n,6,NULL) == CAL_NONE);
    n = build_beacon(f);
    f[46] = 0xff; /* unsupported mandatory rate selector */
    assert(cal_station_receive(&s,f,n,6,NULL) == CAL_NONE);
    n = build_beacon(f);
    assert(cal_station_receive(&s,f,n,6,NULL) == CAL_FOUND);
    assert(s.rate_count == 12 && s.beacon_interval == 100 && !memcmp(s.bssid,ap,6));
    assert(s.ap_rsn_len == sizeof(cal_rsn_ie) && !memcmp(s.ap_rsn,cal_rsn_ie,s.ap_rsn_len));

    assert(!cal_auth_request(&s,out,29) && s.state == CAL_SELECTED);
    assert(cal_auth_request(&s,out,sizeof(out)) == 30 && s.state == CAL_WAIT_AUTH);
    assert(out[0] == 0xb0 && !memcmp(out+10,client,6) && out[26] == 1);
    struct cal_station failure = s;
    reply(f,0xb0,0);
    f[26] = 2;
    f[28] = 13;
    assert(cal_station_receive(&failure,f,30,6,NULL) == CAL_REJECTED);
    assert(failure.last_status == 13 && !cal_assoc_request(&failure,out,sizeof(out)));
    f[28] = 0;
    f[10] ^= 2;
    assert(cal_station_receive(&s,f,30,6,NULL) == CAL_NONE);
    f[10] ^= 2;
    assert(cal_station_receive(&s,f,30,6,NULL) == CAL_AUTHENTICATED);
    assert(!cal_assoc_request(&s,out,25));
    n = cal_assoc_request(&s,out,sizeof(out));
    assert(n == 74 && out[0] == 0 && !memcmp(out + 30,"CalNet",6));
    assert(!memcmp(out + n - sizeof(cal_rsn_ie),cal_rsn_ie,sizeof(cal_rsn_ie)));
    reply(f,0x10,0);
    f[28] = 7;
    f[29] = 0xc0;
    assert(cal_station_receive(&s,f,30,6,NULL) == CAL_ASSOCIATED && s.aid == 7);

    const uint8_t llc[] = {0xaa,0xaa,3,0,0,0,0x88,0x8e};
    reply(f,0x88,2); /* QoS FromDS */
    memcpy(f+26,llc,sizeof(llc));
    uint8_t *e = f + 34;
    e[0] = 2; e[1] = 3; e[3] = 95; e[4] = 2;
    assert(cal_station_receive(&s,f,133,6,&payload) == CAL_EAPOL);
    assert(payload.length == 99 && payload.bytes == e);
    assert(cal_eapol_data(&s,e,99,out,sizeof(out)) == 131);
    assert(out[1] == 1 && !memcmp(out+24,llc,sizeof(llc)) && !memcmp(out+32,e,99));
    e[98] = 1;
    assert(cal_station_receive(&s,f,133,6,&payload) == CAL_NONE && !payload.bytes);
    assert(!cal_eapol_data(&s,e,99,out,sizeof(out)));
    reply(f,0xa0,0);
    f[24] = 8;
    assert(cal_station_receive(&s,f,26,6,NULL) == CAL_DISCONNECTED && s.last_status == 8);
    puts("CalNet station tests passed");
}
