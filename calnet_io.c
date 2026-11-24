#include "calnet_io.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

volatile sig_atomic_t cal_cancelled;
uint64_t cal_millis(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000u; }

int cal_read(struct pipes *p, struct incoming *packet) {
    uint8_t *b = packet->buffer;
    UInt32 n = sizeof(packet->buffer);
    IOReturn status = (*p->usb)->ReadPipeTO(p->usb, p->bulk_in, b, &n, 200, 350);
    if (status == kIOUSBTransactionTimeout || status == kIOReturnTimeout) return 0;
    if (!usb_ok("Station USB receive", status)) return -1;
    if (n < 24 || (b[1] & 0xc0)) return 0;
    size_t offset = 24 + (b[2] & 15) * 8 + (b[3] & 3);
    size_t length = b[0] | ((b[1] & 0x3f) << 8);
    if (offset > n || length < 24 || length > n - offset) return 0;
    packet->descriptor = b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    packet->size = length;
    packet->mpdu = b + offset;
    return 1;
}

int cal_write(void *context, unsigned queue, const uint8_t *data, size_t size) {
    struct pipes *p = context;
    if (queue >= 2 || size == 0 || size > 4096 || !p->max_packet[queue]) return -1;
    UInt8 ref = p->bulk_out[queue];
    if (!usb_ok("Station USB transmit",
        (*p->usb)->WritePipeTO(p->usb, ref, (void *)data, (UInt32)size, 750, 1000))) return -1;
    if (size % p->max_packet[queue] == 0 && !usb_ok("USB short termination",
        (*p->usb)->WritePipeTO(p->usb, ref, (void *)data, 0, 750, 1000))) return -1;
    return 0;
}

bool cal_discover(rtl_radio *radio, struct pipes *p, struct cal_station *station, const char *ssid) {
    // Discovery has no TX path. Replace this local placeholder with the eFuse MAC before TX.
    const uint8_t placeholder[6] = {2,0,0,0,0,1};
    const size_t ssid_len = strlen(ssid);
    cal_station_init_ssid(station, placeholder, (const uint8_t *)ssid, ssid_len);
    const unsigned channels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    unsigned same_name = 0;
    bool diagnosed[14] = {0};
    for (size_t i = 0; i < sizeof(channels)/sizeof(channels[0]) && !cal_cancelled; ++i) {
        unsigned channel = channels[i];
        if (rtl_radio_tune(radio, channel)) return false;
        uint64_t deadline = cal_millis() + 1350;
        while (!cal_cancelled && cal_millis() < deadline) {
            struct incoming packet;
            int rc = cal_read(p, &packet);
            if (rc < 0) return false;
            if (rc == 0) continue;
            if (packet.mpdu[0] == 0x80 && packet.size >= 36) {
                for (size_t j = 36; j + 2 <= packet.size;) {
                    size_t len = packet.mpdu[j+1];
                    if (j + 2 + len > packet.size) break;
                    if (!packet.mpdu[j] && len == ssid_len && !memcmp(packet.mpdu+j+2,ssid,len)) {
                        same_name++;
                        unsigned advertised = 0;
                        char description[640];
                        if (cal_named_beacon_diagnostic(packet.mpdu, packet.size, channel, &advertised,
                                                  description, sizeof(description),(const uint8_t *)ssid,ssid_len) &&
                            advertised < 14 && !diagnosed[advertised]) {
                            puts(description);
                            diagnosed[advertised] = true;
                        }
                    }
                    j += 2 + len;
                }
            }
            struct cal_payload unused;
            if (cal_station_receive(station, packet.mpdu, packet.size, channel, &unused) == CAL_FOUND) {
                printf("%s: compatible 2.4 GHz WPA2-Personal/CCMP beacon, channel %u\n", ssid,channel);
                return true;
            }
        }
    }
    fprintf(stderr, "%s discovery: %u exact-name beacon(s), no compatible selected AP\n", ssid,same_name);
    return false;
}
