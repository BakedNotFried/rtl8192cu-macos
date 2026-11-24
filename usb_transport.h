#pragma once
#include "device.h"

typedef IOUSBInterfaceInterface500 **Interface;
struct pipes {
    Interface usb;
    UInt8 bulk_in, bulk_out[2];
    UInt16 max_packet[2];
    unsigned bulk_out_count;
};
struct usb_task {
    bool (*run)(struct pipes *pipes, void *context);
    void *context;
};
bool use_interface(Device dev, void *context);
bool usb_ok(const char *operation, IOReturn status);
int read_reg(void *ctx, uint16_t reg, uint8_t *buf, size_t len);
int write_reg(void *ctx, uint16_t reg, const uint8_t *buf, size_t len);
