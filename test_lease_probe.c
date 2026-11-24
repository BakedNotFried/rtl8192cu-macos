#include "lease_probe.h"
#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t frame[400], mac[6] = {2,1,2,3,4,5};
    uint32_t xid = 0xd22aae88;
    assert(!dhcp_discover(mac,xid,frame,100));
    size_t len = dhcp_discover(mac,xid,frame,sizeof(frame));
    assert(len == 342 && !dhcp_is_offer(frame,len,mac,xid));
    memcpy(frame,mac,6);
    frame[14+20+1] = 67;
    frame[14+20+3] = 68;
    frame[14+28] = 2;
    frame[14+28+242] = 2;
    assert(dhcp_is_offer(frame,len,mac,xid));
    assert(!dhcp_is_offer(frame,len-1,mac,xid));
    assert(!dhcp_is_offer(frame,len,mac,xid+1));
    frame[14+28+241] = 200;
    assert(!dhcp_is_offer(frame,len,mac,xid));
    return 0;
}
