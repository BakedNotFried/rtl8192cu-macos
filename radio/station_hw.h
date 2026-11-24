#ifndef RTL8192CU_STATION_HW_H
#define RTL8192CU_STATION_HW_H
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue index 0 is the first bulk OUT endpoint (management, 0x02 on this
 * dongle). Index 1 is the second (best-effort data, 0x03). The transport
 * callback completes synchronously, including a terminating short transfer
 * if the byte count is a multiple of the USB bulk max packet size. */
typedef int (*rtl_station_write_bulk)(void *ctx, unsigned queue_index,
                                      const uint8_t *bytes, size_t size);
int rtl_station_prepare(rtl_radio *, const uint8_t bssid[6], unsigned channel,
                        unsigned basic_rate_half_mbps, rtl_station_write_bulk write,
                        void *ctx, uint8_t own_mac[6]);
/* The default entry point selects CalNet. The named entry point pins the
 * exact SSID bytes (1-32) and BSSID for the lifetime of this session. */
int rtl_station_prepare_named(rtl_radio *, const uint8_t bssid[6], unsigned channel,
                              unsigned basic_rate_half_mbps, rtl_station_write_bulk write,
                              void *ctx, uint8_t own_mac[6], const uint8_t *ssid, size_t ssid_len);
int rtl_station_auth(rtl_radio *, const uint8_t *auth_request, size_t size);
int rtl_station_assoc(rtl_radio *, const uint8_t *assoc_request, size_t size);
int rtl_station_link(rtl_radio *, unsigned aid, unsigned beacon_interval);
int rtl_station_eapol(rtl_radio *, const uint8_t *eapol_mpdu, size_t size);

/* Install keys only after authenticating message 3. gtk_rsc is the decoded
 * six-byte packet number (least significant byte first on the wire). */
int rtl_station_keys(rtl_radio *, const uint8_t tk[16], unsigned gtk_id,
                     const uint8_t gtk[16], uint64_t gtk_rsc);
/* Ethernet excludes trailing FCS. TX source must match the eFuse MAC. */
int rtl_station_ethernet(rtl_radio *, const uint8_t *ethernet, size_t size);
/* Decode one received MPDU with its matching little-endian descriptor word
 * zero. The caller has removed USB headers and FCS. Returns 1 for accepted
 * Ethernet, 0 for an irrelevant/replayed frame, -1 on argument/state error. */
int rtl_station_receive_ethernet(rtl_radio *, uint32_t rxdw0,
                                const uint8_t *mpdu, size_t size,
                                uint8_t *ethernet, size_t capacity, size_t *written);
#ifdef __cplusplus
}
#endif
#endif
