#ifndef RTL8192CU_BEACON_H
#define RTL8192CU_BEACON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Count structurally valid infrastructure beacons in one RTL8192CU DMA
 * aggregate (128-byte alignment). Set includes_fcs from the configured
 * receive mode. target_bssid can be NULL or point to six bytes.
 * On malformed framing return false and leave *count at zero. */
bool rtl8192cu_count_beacons(const uint8_t *transfer, size_t length,
                             bool includes_fcs, const uint8_t *target_bssid,
                             unsigned *count);

/* For RX with DMA and USB aggregation both disabled: a single record per
 * transfer. RXDW2's aggregate count has no meaning in this configuration. */
bool rtl8192cu_count_single_beacon(const uint8_t *transfer, size_t length,
                                   bool includes_fcs, const uint8_t *target_bssid,
                                   unsigned *count);

#endif
