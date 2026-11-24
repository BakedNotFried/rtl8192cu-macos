#ifndef CAL_WPA_H
#define CAL_WPA_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cal_wpa cal_wpa;
struct cal_wpa_ops {
    void *context;
    /* Both callbacks return zero on success. EAPOL starts with its version byte. */
    int (*send_eapol)(void *context, const uint8_t *frame, size_t length);
    /* Pairwise key ID is zero. RSC is six bytes in little-endian PN order. */
    int (*install_ccmp)(void *context, int pairwise, unsigned key_id,
                        const uint8_t key[16], const uint8_t rsc[6]);
};

/* Start after successful association to CalNet. Passphrase is 8-63 ASCII bytes
 * or 64 ASCII hex digits for a raw PMK. Caller retains and clears its buffer.
 * Both IEs include the element ID and length. Call on a single thread. */
cal_wpa *cal_wpa_start(const struct cal_wpa_ops *ops, const uint8_t sta[6],
                       const uint8_t ap[6], const uint8_t *assoc_rsn, size_t assoc_len,
                       const uint8_t *ap_rsn, size_t ap_len, const uint8_t *secret, size_t secret_len);
/* For association to a specific configured WPA2-Personal SSID (1-32 octets).
 * Caller must match this SSID to the selected AP before transmitting. */
cal_wpa *cal_wpa_start_named(const struct cal_wpa_ops *ops, const uint8_t sta[6],
                             const uint8_t ap[6], const uint8_t *assoc_rsn, size_t assoc_len,
                             const uint8_t *ap_rsn, size_t ap_len, const uint8_t *secret,
                             size_t secret_len, const uint8_t *ssid, size_t ssid_len);
/* -1 indicates failure, 0 waiting, 1 authenticated with both temporal keys installed.
 * protected_frame indicates the input EAPOL frame was decrypted and authenticated. */
int cal_wpa_receive(cal_wpa *wpa, const uint8_t src[6], const uint8_t *frame,
                    size_t length, int protected_frame);
void cal_wpa_stop(cal_wpa *wpa);
#ifdef __cplusplus
}
#endif
#endif
