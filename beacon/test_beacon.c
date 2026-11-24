#include "beacon.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 802.11 beacon for a made-up basic service set: timestamp, 100 TU
 * interval, ESS capability, hidden SSID, supported rates and DS channel. */
static const uint8_t example[] = {
    0x80, 0, 0, 0, 255,255,255,255,255,255,
    2,0,0,0,0,1, 2,0,0,0,0,1, 0x10,0,
    1,2,3,4,5,6,7,8, 100,0, 1,0,
    0,0, 1,2,0x82,0x84, 3,1,6
};

static size_t record(uint8_t *p, unsigned count, size_t info_units,
                     size_t shift, bool fcs)
{
    size_t size = sizeof(example) + (fcs ? 4u : 0u);
    p[0] = (uint8_t)size;
    p[2] = (uint8_t)info_units;
    p[3] = (uint8_t)(0x30 | shift);
    p[10] = (uint8_t)count;
    size_t data = 24 + info_units * 8 + shift;
    memcpy(p + data, example, sizeof(example));
    return data + size;
}

int main(void)
{
    uint8_t aggregate[3 * 128] = {0};
    unsigned n = 0;
    size_t first = record(aggregate, 3, 4, 0, true);
    size_t second = record(aggregate + 128, 0, 1, 1, true);
    size_t final = record(aggregate + 256, 0, 0, 0, true);
    size_t total = 256 + final;
    assert(first < 128 && second < 128);
    assert(rtl8192cu_count_beacons(aggregate, total, true, example + 16, &n) && n == 3);
    uint8_t other[6] = {2,0,0,0,0,2};
    assert(rtl8192cu_count_beacons(aggregate, total, true, other, &n) && n == 0);
    /* CRC error, ICV error and a data frame all fail the positive test. */
    aggregate[1] = 0x40;
    aggregate[129] = 0x80;
    aggregate[256 + 24] = 0x08;
    assert(rtl8192cu_count_beacons(aggregate, total, true, NULL, &n) && n == 0);
    aggregate[1] = 0;
    /* No partial success on truncated transfer, even after a good record. */
    for (size_t length = 0; length < total; length++)
        assert(!rtl8192cu_count_beacons(aggregate, length, true, NULL, &n) && n == 0);
    aggregate[10] = 0;
    assert(!rtl8192cu_count_beacons(aggregate, total, true, NULL, &n) && n == 0);
    aggregate[10] = 255;
    assert(!rtl8192cu_count_beacons(aggregate, total, true, NULL, &n) && n == 0);
    uint8_t single[128] = {0};
    size_t size = record(single, 1, 0, 0, false);
    assert(rtl8192cu_count_beacons(single, size, false, NULL, &n) && n == 1);
    single[10] = 0;
    assert(rtl8192cu_count_single_beacon(single, size, false, NULL, &n) && n == 1);
    assert(!rtl8192cu_count_beacons(single, size, false, NULL, &n) && n == 0);
    assert(!rtl8192cu_count_single_beacon(single, size - 1, false, NULL, &n) && n == 0);
    single[10] = 255;
    assert(rtl8192cu_count_single_beacon(single, size, false, NULL, &n) && n == 1);
    single[10] = 1;
    single[size - 2] = 9; /* IE claims more bytes than the frame contains. */
    assert(rtl8192cu_count_beacons(single, size, false, NULL, &n) && n == 0);
    single[0] = 0;
    assert(!rtl8192cu_count_beacons(single, size, false, NULL, &n) && n == 0);
    puts("RTL8192CU beacon aggregate tests passed");
    return 0;
}
