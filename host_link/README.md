# Temporary macOS Ethernet link

The host-link helper creates a pair of virtual Ethernet interfaces for one foreground station session. macOS obtains an IPv4 address, router and DNS configuration on the host end using DHCP. The host end uses the dongle’s station MAC. Traffic to and from the radio passes through the other end of the pair. This uses Apple's `feth` test driver. Availability and behavior can vary with macOS versions. [Apple XNU driver](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/net/if_fake.c) · [Apple interface pairing command](https://github.com/apple-oss-distributions/network_cmds/blob/main/ifconfig.tproj/iffake.c)

The privileged supervisor creates kernel-assigned interface names and opens BPF on the new peer. Once setup succeeds, a worker drops to the station user's UID and GID and passes IPv4 and ARP frames over a private local stream. The BPF descriptor stays inside the helper. The workstation chooses its route according to its active network configuration. An interface-bound IPv4 test is needed to verify that traffic used the dongle when other connections are active.

The worker inherits an open BPF descriptor. The macOS 27.2 SDK and Apple's published XNU implementation provide no `BIOCLOCK` operation to freeze its configuration. The interface binding is enforced by helper code. Only the supervisor selects a peer, and the worker uses the descriptor for frame reads and writes. The station receives framed Ethernet data over IPC. [Apple XNU BPF implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/net/bpf.c)

## Teardown

Stop the foreground station with Control-C. The station closes its connection to the helper. The supervisor stops DHCP on the host interface and destroys the two interfaces it created. It checks each interface's index before teardown. Failure partway through setup follows the same cleanup path.

A force-quit or system crash can interrupt cleanup. If an interface remains, examine the two names from the supervisor's `Temporary host interface … and peer …` message. Use `/sbin/ifconfig NAME` to verify each still exists and that its reported peer is the other interface from that session. After checking both identities, stop the session's temporary IPv4 service with `sudo /usr/sbin/ipconfig set HOST_NAME NONE`. Then remove that pair with `sudo /sbin/ifconfig PEER_NAME destroy` followed by `sudo /sbin/ifconfig HOST_NAME destroy`. The names are assigned afresh on every run.

## Verification

`test_helper.c` uses pipes and a local socket pair to test frame filtering, MAC direction, bounds and message framing. It runs as a regular user. A live test requires administrator approval for creating the temporary link, a supported dongle and an associated WPA2 station.
