#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach_error.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "device.h"

static bool check(const char *operation, IOReturn status) {
    if (status == kIOReturnSuccess) return true;
    fprintf(stderr, "%s: %s (0x%08x)\n", operation, mach_error_string(status),
            (unsigned)status);
    return false;
}

static void match_number(CFMutableDictionaryRef match, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &value);
    CFDictionarySetValue(match, key, number);
    CFRelease(number);
}

static bool describe(Device dev) {
    IOUSBConfigurationDescriptorPtr config = NULL;
    if (!check("USB configuration descriptor",
               (*dev)->GetConfigurationDescriptorPtr(dev, 0, &config)) || !config)
        return false;
    const uint8_t *p = (const uint8_t *)config;
    size_t remaining = USBToHostWord(config->wTotalLength);
    if (remaining < sizeof(*config)) return false;
    printf("Configuration %u, %u interface(s)\n", config->bConfigurationValue,
           config->bNumInterfaces);
    while (remaining > 0) {
        if (remaining < 2 || p[0] < 2 || p[0] > remaining) {
            fprintf(stderr, "Malformed USB configuration descriptor\n");
            return false;
        }
        if (p[1] == kUSBInterfaceDesc && p[0] >= 9)
            printf("  interface %u alt %u class %02x/%02x/%02x\n",
                   p[2], p[3], p[5], p[6], p[7]);
        if (p[1] == kUSBEndpointDesc && p[0] >= 7)
            printf("    endpoint 0x%02x type %u max packet %u\n",
                   p[2], p[3] & 3, p[4] | (p[5] << 8));
        remaining -= p[0];
        p += p[0];
    }
    return true;
}

// RTL8192CU vendor control-IN request, as in OpenBSD urtwn_read_region_1.
static bool read_register(Device dev, uint16_t address, uint32_t *value) {
    uint32_t little_endian = 0;
    IOUSBDevRequestTO request = {
        .bmRequestType = 0xc0, .bRequest = 0x05, .wValue = address,
        .wIndex = 0, .wLength = sizeof(little_endian), .pData = &little_endian,
        .noDataTimeout = 1000, .completionTimeout = 2000
    };
    if (!check("USB control-IN register read", (*dev)->DeviceRequestTO(dev, &request)))
        return false;
    if (request.wLenDone != sizeof(little_endian)) {
        fprintf(stderr, "Short register read at 0x%04x (%u bytes)\n",
                address, (unsigned)request.wLenDone);
        return false;
    }
    *value = OSSwapLittleToHostInt32(little_endian);
    return true;
}

static bool identify(Device dev) {
    uint32_t sys_cfg, bonding;
    if (!read_register(dev, 0x00f0, &sys_cfg)) return false;
    printf("SYS_CFG = 0x%08x\n", sys_cfg);
    if ((sys_cfg & 0x00800000) || !(sys_cfg & 0x08000000)) {
        fprintf(stderr, "Expected production RTL8192C silicon; identification stopped\n");
        return false;
    }
    if (!read_register(dev, 0x00ec, &bonding) || bonding == UINT32_MAX) return false;
    printf("HPON_FSM = 0x%08x\n", bonding);
    printf("RTL8192CU: foundry %s, cut field %u, radio %s\n",
           sys_cfg & 0x00080000 ? "UMC" : "TSMC", (sys_cfg >> 12) & 0xf,
           ((bonding >> 22) & 3) == 1 ? "1T2R" : "2T2R");
    return true;
}

int with_identified_adapter(bool (*run)(Device, void *), void *context) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return 1;
    match_number(match, CFSTR("idVendor"), 0x0bda);
    match_number(match, CFSTR("idProduct"), 0x8178);
    io_iterator_t matches;
    if (!check("USB device enumeration",
               IOServiceGetMatchingServices(kIOMainPortDefault, match, &matches))) return 1;
    io_service_t service = IOIteratorNext(matches);
    io_service_t extra = IOIteratorNext(matches);
    IOObjectRelease(matches);
    if (!service || extra) {
        fprintf(stderr, "Expected exactly one connected 0bda:8178 adapter\n");
        if (service) IOObjectRelease(service);
        if (extra) IOObjectRelease(extra);
        return 1;
    }
    puts("Matched USB 0bda:8178");
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn status = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    IOObjectRelease(service);
    if (!check("Create USB device user client", status) || !plugin) return 1;
    Device dev = NULL;
    HRESULT result = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID *)&dev);
    (*plugin)->Release(plugin);
    if (result || !dev) {
        fprintf(stderr, "Query USB device interface failed: 0x%08x\n", (unsigned)result);
        return 1;
    }
    bool ok = describe(dev) && identify(dev);
    if (ok && run) ok = run(dev, context);
    (*dev)->Release(dev);
    return ok ? 0 : 1;
}

#ifndef RTL_BEACON_TOOL
int main(void) {
    return with_identified_adapter(NULL, NULL);
}
#endif
