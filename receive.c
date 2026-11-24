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
#include "beacon/beacon.h"

#include "usb_transport.h"

struct test {
    unsigned channel;
    const uint8_t *bssid;
};

static volatile sig_atomic_t interrupted;
static void cancel(int signo) {
    (void)signo;
    interrupted = 1;
}

// RTL8192C RXERR_RPT is a multiplexed hardware counter. The selector is the
// upper nibble. Write only that byte, with the counter-reset bit always clear.
static void report_counts(Interface usb) {
    enum { report = 0x664 };
    static const struct { uint8_t select; const char *name; } counters[] = {
        {0, "ofdm-phy"}, {2, "ofdm-good"}, {3, "ofdm-crc"},
        {4, "cck-phy"}, {6, "cck-good"}, {7, "cck-crc"},
        {8, "ht-phy"}, {11, "ht-good"}, {12, "ht-crc"}, {15, "fifo-drop"}
    };
    uint8_t original;
    if (read_reg(usb, report + 3, &original, 1)) return;
    original &= 0xf0;
    printf("Hardware RX counters:");
    for (size_t i = 0; i < sizeof(counters)/sizeof(counters[0]); ++i) {
        uint8_t selector = counters[i].select << 4;
        uint8_t bytes[4];
        if (write_reg(usb, report + 3, &selector, 1) ||
            read_reg(usb, report, bytes, sizeof(bytes))) break;
        unsigned value = bytes[0] | (bytes[1] << 8) | ((bytes[2] & 15) << 16);
        printf(" %s=%u", counters[i].name, value);
    }
    puts("");
    (void)write_reg(usb, report + 3, &original, 1);
}

enum receive_result { receive_error, channel_quiet, beacon_seen };

static enum receive_result listen_on_channel(struct pipes *p, const struct test *test,
                                             unsigned channel) {
    uint8_t buffer[16 * 1024];
    uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    unsigned transfers = 0, malformed = 0;
    while (!interrupted && clock_gettime_nsec_np(CLOCK_MONOTONIC) - start < 1800000000ull) {
        UInt32 size = sizeof(buffer);
        IOReturn status = (*p->usb)->ReadPipeTO(p->usb, p->bulk_in, buffer, &size, 400, 600);
        if (status == kIOUSBTransactionTimeout || status == kIOReturnTimeout) continue;
        if (!usb_ok("Bulk input", status)) return receive_error;
        ++transfers;
        unsigned count = 0;
        if (!rtl8192cu_count_single_beacon(buffer, size, false, test->bssid, &count)) {
            if (!malformed && size >= 24) {
                printf("Unexpected RX descriptor: USB bytes=%u packet=%u info=%u shift=%u "
                       "aggregate count=%u CRC/ICV flags=%u\n", (unsigned)size,
                       buffer[0] | ((buffer[1] & 0x3f) << 8), (buffer[2] & 15) * 8,
                       buffer[3] & 3, buffer[10], buffer[1] >> 6);
            }
            ++malformed;
        }
        if (count) {
            printf("Beacon reception verified: %u valid beacon(s), requested channel %u\n",
                   count, channel);
            return beacon_seen;
        }
    }
    printf("Channel %u: %u USB transfer(s), %u malformed, no matching beacon\n",
           channel, transfers, malformed);
    return channel_quiet;
}

static bool run_radio(struct pipes *p, void *context) {
    const struct test *test = context;
    struct rtl_radio_io io = {.ctx = p->usb, .read = read_reg, .write = write_reg};
    rtl_radio *radio = rtl_radio_create(io);
    if (!radio) return false;
    unsigned channel = test->channel ? test->channel : 1;
    bool ok = rtl_radio_start(radio, p->bulk_out_count, channel) == 0;
    printf("Firmware ready indication: %s\n", rtl_radio_firmware_ready(radio) ? "yes" : "no");
    if (!ok) fprintf(stderr, "Radio initialization: %s\n", rtl_radio_error(radio));
    char snapshot[768];
    if (ok && !rtl_radio_snapshot(radio, snapshot, sizeof(snapshot)))
        printf("Receiver before listening: %s\n", snapshot);
    if (ok) report_counts(p->usb);
    bool received = false;
    static const unsigned channels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    for (size_t i = 0; ok && !interrupted && i < sizeof(channels)/sizeof(channels[0]); ++i) {
        if (i > 0) {
            if (test->channel) break;
            channel = channels[i];
            if (rtl_radio_tune(radio, channel)) {
                fprintf(stderr, "Channel tuning: %s\n", rtl_radio_error(radio));
                break;
            }
        }
        enum receive_result result = listen_on_channel(p, test, channel);
        received = result == beacon_seen;
        if (result != channel_quiet) break;
    }
    if (ok && !rtl_radio_snapshot(radio, snapshot, sizeof(snapshot)))
        printf("Receiver after listening: %s\n", snapshot);
    if (ok) report_counts(p->usb);
    bool stopped = rtl_radio_stop(radio) == 0;
    if (!stopped) fprintf(stderr, "Radio shutdown: %s\n", rtl_radio_error(radio));
    rtl_radio_destroy(radio);
    return received && stopped;
}

int main(int argc, char **argv) {
    struct test test = {0};
    uint8_t target[6];
    if (argc > 3) goto usage;
    if (argc > 1) {
        char *end;
        long channel = strtol(argv[1], &end, 10);
        if (*end || channel < 1 || channel > 13) goto usage;
        test.channel = (unsigned)channel;
    }
    if (argc > 2) {
        if (strlen(argv[2]) != 17) goto usage;
        for (size_t i = 0; i < 6; ++i) {
            const char *octet = argv[2] + 3 * i;
            if (strspn(octet, "0123456789abcdefABCDEF") < 2 ||
                (i < 5 && octet[2] != ':')) goto usage;
            char pair[] = {octet[0], octet[1], 0};
            target[i] = (uint8_t)strtoul(pair, NULL, 16);
        }
        test.bssid = target;
    }
    signal(SIGINT, cancel);
    signal(SIGTERM, cancel);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct usb_task task = {run_radio, &test};
    return with_identified_adapter(use_interface, &task);
usage:
    fprintf(stderr, "Usage: %s [channel 1-13 [test BSSID]]\n", argv[0]);
    return 2;
}
