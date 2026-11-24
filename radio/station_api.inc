// Public typed station operations. All calls use the same guarded error path.
extern "C" int rtl_station_prepare(rtl_radio *r, const uint8_t bssid[6], unsigned channel,
    unsigned rate, rtl_station_write_bulk write, void *ctx, uint8_t own[6]) {
    static constexpr uint8_t default_ssid[] = {'C','a','l','N','e','t'};
    return rtl_station_prepare_named(r,bssid,channel,rate,write,ctx,own,default_ssid,sizeof(default_ssid));
}
extern "C" int rtl_station_prepare_named(rtl_radio *r, const uint8_t bssid[6], unsigned channel,
    unsigned rate, rtl_station_write_bulk write, void *ctx, uint8_t own[6], const uint8_t *ssid, size_t len) {
    return guarded(r,[&] { r->prepare_station(bssid,channel,rate,write,ctx,own,ssid,len); });
}
extern "C" int rtl_station_auth(rtl_radio *r, const uint8_t *p, size_t n) {
    return guarded(r,[&] { r->send_auth(p,n); });
}
extern "C" int rtl_station_assoc(rtl_radio *r, const uint8_t *p, size_t n) {
    return guarded(r,[&] { r->send_assoc(p,n); });
}
extern "C" int rtl_station_link(rtl_radio *r, unsigned aid, unsigned interval) {
    return guarded(r,[&] { r->station_link(aid,interval); });
}
extern "C" int rtl_station_eapol(rtl_radio *r, const uint8_t *p, size_t n) {
    return guarded(r,[&] { r->send_eapol(p,n); });
}
extern "C" int rtl_station_keys(rtl_radio *r, const uint8_t tk[16], unsigned id, const uint8_t gtk[16], uint64_t rsc) {
    return guarded(r,[&] { r->station_keys(tk,id,gtk,rsc); });
}
extern "C" int rtl_station_ethernet(rtl_radio *r, const uint8_t *p, size_t n) {
    return guarded(r,[&] { r->send_ethernet(p,n); });
}
extern "C" int rtl_station_receive_ethernet(rtl_radio *r, uint32_t dw0, const uint8_t *p,
    size_t n, uint8_t *eth, size_t cap, size_t *written) {
    bool accepted = false;
    if (guarded(r,[&] { accepted = r->receive_ethernet(dw0,p,n,eth,cap,written); })) return -1;
    return accepted ? 1 : 0;
}
