# CalNet hardware trial

On 25 November 2026 the Realtek RTL8192CU dongle (`0bda:8178`) joined CalNet over 2.4 GHz and received a DHCP offer over the encrypted link. The test host was a 16-inch M1 Max MacBook Pro (`MacBookPro18,2`) running macOS 27.2.1. The two runs below used different security settings under the same network name.

| CalNet configuration | Observation |
| --- | --- |
| WPA3-Personal on channel 2 | One discovery pass counted 42 beacons advertising the exact SSID. The AP advertised SAE, CCMP and required protected management frames. The station exited during discovery, before association or password access. |
| WPA2-Personal on channel 2 | The dongle found a compatible PSK/CCMP beacon, completed Open System authentication and association, then completed the WPA2 four-way handshake and installed CCMP keys. A DHCP server answered the dongle's encrypted DHCPDISCOVER. The prototype decrypted the offer and matched its transaction ID and client MAC before shutting down the adapter. |

The DHCP exchange verified traffic in both directions through the physical dongle. The trial ended at the offer stage. A persistent network interface, an assigned IP address and internet access from macOS applications fall outside the evidence from this trial.

## Repeat the test

Run the following from the repository root with the dongle attached. The access point must advertise the exact SSID on 2.4 GHz. For the join test it must offer WPA2-Personal with CCMP, allow a station without protected management frames, and have a reachable DHCPv4 server.

```sh
station="$(sh build-station.sh)" && "$station" --discover 'CalNet'
```

After discovery reports a compatible beacon, use the same Terminal window to run the join. Enter the password in the local macOS dialog.

```sh
"$station" --join 'CalNet'
```

To use an existing saved CalNet credential instead, select the Keychain mode for the join.

```sh
"$station" --join-saved 'CalNet'
```

A successful run ends with `Encrypted bidirectional data verified by a matching DHCP offer`. For a management-only check, run `"$station" --associate 'CalNet'`. [Project setup and troubleshooting](../../README.md)
