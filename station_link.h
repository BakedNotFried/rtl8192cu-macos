#ifndef RTL8192CU_STATION_LINK_H
#define RTL8192CU_STATION_LINK_H
#include "calnet_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All operations run serially on the radio thread. up() runs once after key
 * installation. It must clean up its own partial setup on failure. down()
 * runs once after a successful up(), including on AP/USB/backend failure.
 * Both up() and write_frame() return zero on success.
 *
 * read_frame() is nonblocking, returning 1 for one complete frame, 0 when
 * idle, -1 on failure. Ethernet frames omit FCS and use a 1500-byte MTU.
 * Host frames use the MAC provided by up(). should_stop() is optional. */
struct cal_link_ops {
    void *ctx;
    int (*up)(void *ctx, const uint8_t mac[6]);
    int (*read_frame)(void *ctx, uint8_t *frame, size_t capacity, size_t *length);
    int (*write_frame)(void *ctx, const uint8_t *frame, size_t length);
    void (*down)(void *ctx);
    bool (*should_stop)(void *ctx);
};

/* Enter with a live device and a compatible exact-name AP selected. Caller
 * owns the device and stops the radio when this function returns. True means
 * requested shutdown. AP disconnect, failed authentication, and data-path
 * failures return false. use_saved controls keychain vs terminal prompt. */
bool cal_join_persistent(rtl_radio *radio, struct pipes *usb, struct cal_station *station,
                         bool use_saved, const struct cal_link_ops *link);

#ifdef __cplusplus
}
#endif
#endif
