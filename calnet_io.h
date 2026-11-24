#pragma once
#include "usb_transport.h"
#include "station/station.h"
#include "radio/radio.h"
#include <signal.h>
extern volatile sig_atomic_t cal_cancelled;
struct incoming {
    uint8_t buffer[16384];
    const uint8_t *mpdu;
    size_t size;
    uint32_t descriptor;
};
// 1 for a valid packet, 0 for timeout or discarded packet, -1 for USB error.
int cal_read(struct pipes *p, struct incoming *packet);
int cal_read_for(struct pipes *p, struct incoming *packet, unsigned idle_ms, unsigned total_ms);
int cal_write(void *context, unsigned queue, const uint8_t *data, size_t size);
uint64_t cal_millis(void);
bool cal_discover(rtl_radio *radio, struct pipes *p, struct cal_station *station, const char *ssid);
