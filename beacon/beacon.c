#include "beacon.h"

#include <string.h>

/* Descriptor layout and DMA alignment are documented in the bundled OpenBSD
 * sources radio/upstream/r92creg.h and if_urtwn.c (urtwn_rxeof). */
enum { descriptor_size = 24, dma_alignment = 128, beacon_fixed_size = 36 };

static bool infrastructure_beacon(const uint8_t *frame, size_t length,
                                  const uint8_t *target_bssid)
{
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    static const uint8_t zero[6] = {0};

    if (length < beacon_fixed_size || frame[0] != 0x80 ||
        (frame[1] & 0xc7) != 0 || (frame[22] & 0x0f) != 0)
        return false;
    /* A conventional infrastructure AP uses its BSSID as the transmitter,
     * addresses the beacon to broadcast, and sets the ESS capability bit. */
    const uint8_t *bssid = frame + 16;
    if (memcmp(frame + 4, broadcast, 6) != 0 || memcmp(frame + 10, bssid, 6) != 0 ||
        (bssid[0] & 1) != 0 || memcmp(bssid, zero, 6) == 0 ||
        (target_bssid && memcmp(bssid, target_bssid, 6) != 0) ||
        (frame[32] == 0 && frame[33] == 0) || (frame[34] & 3) != 1)
        return false;

    bool has_ssid = false;
    size_t offset = beacon_fixed_size;
    while (offset < length) {
        if (length - offset < 2)
            return false;
        uint8_t tag = frame[offset++];
        size_t tag_length = frame[offset++];
        if (tag_length > length - offset)
            return false;
        if (tag == 0) {
            if (has_ssid || tag_length > 32)
                return false;
            has_ssid = true; /* A hidden SSID can have zero length. */
        }
        offset += tag_length;
    }
    return has_ssid;
}

static bool count_transfer(const uint8_t *transfer, size_t length, bool aggregated,
                           bool includes_fcs, const uint8_t *target_bssid,
                           unsigned *count)
{
    if (!count)
        return false;
    *count = 0;
    if (!transfer || length < descriptor_size)
        return false;

    /* RXDW2[23:16] counts packets only in aggregated transfers. */
    unsigned records = aggregated ? transfer[10] : 1;
    if (records == 0)
        return false;
    unsigned matches = 0;
    for (unsigned i = 0; i < records; i++) {
        if (length < descriptor_size)
            return false;
        size_t packet_size = transfer[0] | ((size_t)(transfer[1] & 0x3f) << 8);
        size_t driver_info = (transfer[2] & 0x0f) * 8;
        size_t shift = transfer[3] & 3;
        size_t packet_offset = descriptor_size + driver_info + shift;
        size_t used = packet_offset + packet_size;
        if (packet_size == 0 || used > length)
            return false;
        /* CRC and ICV status bits are supplied by the receiver. */
        size_t fcs = includes_fcs ? 4u : 0u;
        if ((transfer[1] & 0xc0) == 0 && packet_size >= fcs &&
            infrastructure_beacon(transfer + packet_offset, packet_size - fcs, target_bssid))
            matches++;

        if (i + 1 < records) {
            size_t next = (used + dma_alignment - 1) & ~(size_t)(dma_alignment - 1);
            if (next > length)
                return false;
            transfer += next;
            length -= next;
        }
    }
    *count = matches;
    return true;
}

bool rtl8192cu_count_beacons(const uint8_t *transfer, size_t length,
                             bool includes_fcs, const uint8_t *target_bssid,
                             unsigned *count)
{
    return count_transfer(transfer, length, true, includes_fcs, target_bssid, count);
}

bool rtl8192cu_count_single_beacon(const uint8_t *transfer, size_t length,
                                   bool includes_fcs, const uint8_t *target_bssid,
                                   unsigned *count)
{
    return count_transfer(transfer, length, false, includes_fcs, target_bssid, count);
}
