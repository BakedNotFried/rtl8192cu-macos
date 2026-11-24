#ifndef RTL8192CU_RADIO_H
#define RTL8192CU_RADIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport must claim only 0bda:8178; complete the entire transfer or return
 * an error. No bulk-out path is used. Reads/writes address volatile registers
 * via USB control endpoint 0. The caller owns ctx and serializes all calls. */
struct rtl_radio_io {
    void *ctx;
    int (*read)(void *ctx, uint16_t reg, uint8_t *bytes, size_t count);
    int (*write)(void *ctx, uint16_t reg, const uint8_t *bytes, size_t count);
};
typedef struct rtl_radio rtl_radio;
rtl_radio *rtl_radio_create(struct rtl_radio_io io);
int rtl_radio_start(rtl_radio *radio, unsigned bulk_out_count, unsigned channel);
int rtl_radio_tune(rtl_radio *radio, unsigned channel);
/* After a partial initialization attempt, call stop before releasing USB. */
int rtl_radio_stop(rtl_radio *radio);
const char *rtl_radio_error(const rtl_radio *radio);
int rtl_radio_firmware_ready(const rtl_radio *radio);
/* Register snapshot of the running receiver, without reading packet data.
 * Provide a buffer of at least 512 bytes. Returns 0 on success. */
int rtl_radio_snapshot(rtl_radio *radio, char *text, size_t size);
void rtl_radio_destroy(rtl_radio *radio);

#ifdef __cplusplus
}
#endif
#endif
