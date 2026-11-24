#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <mach/mach_error.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "device.h"
#include "radio/radio.h"
#include "usb_transport.h"

bool usb_ok(const char *operation, IOReturn status) {
    if (status == kIOReturnSuccess) return true;
    fprintf(stderr, "%s: %s (0x%08x)\n", operation, mach_error_string(status),
            (unsigned)status);
    return false;
}

static Interface find_interface(Device dev) {
    IOUSBFindInterfaceRequest match = {
        .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff,
        .bInterfaceProtocol = 0xff, .bAlternateSetting = 0
    };
    io_iterator_t interfaces;
    if (!usb_ok("Find interface", (*dev)->CreateInterfaceIterator(dev, &match, &interfaces)))
        return NULL;
    io_service_t service = IOIteratorNext(interfaces);
    io_service_t other = IOIteratorNext(interfaces);
    IOObjectRelease(interfaces);
    if (!service || other) {
        if (service) IOObjectRelease(service);
        if (other) IOObjectRelease(other);
        fprintf(stderr, "Expected one vendor interface\n");
        return NULL;
    }
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn status = IOCreatePlugInInterfaceForService(
        service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (!usb_ok("Create USB interface user client", status) || !plugin) return NULL;
    Interface usb = NULL;
    HRESULT result = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID *)&usb);
    (*plugin)->Release(plugin);
    if (result || !usb) {
        fprintf(stderr, "Query USB interface failed: 0x%08x\n", (unsigned)result);
        return NULL;
    }
    return usb;
}

static bool discover_pipes(struct pipes *p) {
    UInt8 count;
    if (!usb_ok("Count endpoints", (*p->usb)->GetNumEndpoints(p->usb, &count))) return false;
    for (unsigned i = 1; i <= count; ++i) {
        UInt8 direction, number, type, interval;
        UInt16 packet_size;
        if (!usb_ok("Read endpoint", (*p->usb)->GetPipeProperties(
                p->usb, i, &direction, &number, &type, &packet_size, &interval))) return false;
        if (type == kUSBBulk && direction == kUSBIn && number == 1) p->bulk_in = i;
        if (type == kUSBBulk && direction == kUSBOut) {
            ++p->bulk_out_count;
            if (number == 2 || number == 3) {
                p->bulk_out[number - 2] = i;
                p->max_packet[number - 2] = packet_size;
            }
        }
    }
    if (p->bulk_in && p->bulk_out_count == 2 && p->bulk_out[0] && p->bulk_out[1]) return true;
    fprintf(stderr, "Unexpected USB pipe layout for the qualified adapter\n");
    return false;
}

static int reg_transfer(Interface usb, bool read, uint16_t reg, void *buf, size_t len) {
    if (!len || len > UINT16_MAX) return -1;
    IOUSBDevRequestTO req = {
        .bmRequestType = read ? 0xc0 : 0x40, .bRequest = 5, .wValue = reg,
        .wLength = (UInt16)len, .pData = buf, .noDataTimeout = 1000, .completionTimeout = 2000
    };
    IOReturn result = (*usb)->ControlRequestTO(usb, 0, &req);
    if (result || req.wLenDone != len) {
        fprintf(stderr, "Register %s 0x%04x: status 0x%08x, %u/%zu bytes\n",
                read ? "read" : "write", reg, (unsigned)result, (unsigned)req.wLenDone, len);
        return -1;
    }
    return 0;
}
int read_reg(void *ctx, uint16_t reg, uint8_t *buf, size_t len) {
    return reg_transfer(ctx, true, reg, buf, len);
}
int write_reg(void *ctx, uint16_t reg, const uint8_t *buf, size_t len) {
    return reg_transfer(ctx, false, reg, (void *)buf, len);
}

bool use_interface(Device dev, void *context) {
    Interface usb = find_interface(dev);
    if (!usb) return false;
    bool ok = usb_ok("Open USB interface", (*usb)->USBInterfaceOpen(usb));
    if (ok) {
        struct pipes p = {.usb = usb};
        struct usb_task *task = context;
        ok = discover_pipes(&p) && task->run(&p, task->context);
        if (!usb_ok("Close USB interface", (*usb)->USBInterfaceClose(usb))) ok = false;
    }
    (*usb)->Release(usb);
    return ok;
}

