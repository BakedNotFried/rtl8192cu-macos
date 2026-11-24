#ifndef CAL_STATION_H
#define CAL_STATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum cal_state { CAL_SCANNING, CAL_SELECTED, CAL_WAIT_AUTH, CAL_AUTH_OK,
                 CAL_WAIT_ASSOC, CAL_ASSOC_OK, CAL_FAILED };
enum cal_event { CAL_NONE, CAL_FOUND, CAL_AUTHENTICATED, CAL_ASSOCIATED,
                 CAL_REJECTED, CAL_EAPOL, CAL_DISCONNECTED };
struct cal_payload { const uint8_t *bytes; size_t length; };
struct cal_station {
    uint8_t mac[6], bssid[6], channel, rates[16], rate_count;
    uint8_t ssid[32], ssid_len;
    uint16_t aid, sequence, capability, last_status, beacon_interval;
    uint8_t ap_rsn[257];
    size_t ap_rsn_len;
    enum cal_state state;
};
extern const uint8_t cal_rsn_ie[22];
void cal_station_init(struct cal_station *station, const uint8_t mac[6]);
bool cal_station_init_ssid(struct cal_station *station, const uint8_t mac[6],
                           const uint8_t *ssid, size_t ssid_len);
/* Return diagnostic metadata only for a beacon advertising the exact SSID. */
bool cal_beacon_diagnostic(const uint8_t *mpdu, size_t length, unsigned listening_channel,
                           unsigned *advertised_channel, char *message, size_t capacity);
bool cal_named_beacon_diagnostic(const uint8_t *mpdu, size_t length, unsigned listening_channel,
                                 unsigned *advertised_channel, char *message, size_t capacity,
                                 const uint8_t *ssid, size_t ssid_len);
/* Input is a CRC-checked, complete MPDU with descriptor and FCS stripped.
 * CAL_EAPOL returns a borrowed view valid only during the RX buffer lifetime. */
enum cal_event cal_station_receive(struct cal_station *station, const uint8_t *mpdu,
                                  size_t length, unsigned channel, struct cal_payload *eapol);
size_t cal_auth_request(struct cal_station *station, uint8_t *out, size_t cap);
size_t cal_assoc_request(struct cal_station *station, uint8_t *out, size_t cap);
size_t cal_eapol_data(struct cal_station *station, const uint8_t *eapol, size_t length,
                      uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
