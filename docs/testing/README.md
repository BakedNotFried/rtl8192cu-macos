# CalNet hardware trial

On 25 November 2026 the Realtek RTL8192CU dongle (`0bda:8178`) joined CalNet over 2.4 GHz and received a DHCP offer over the encrypted link. The test host was a 16-inch M1 Max MacBook Pro (`MacBookPro18,2`) running macOS 27.2.1. The two runs below used different security settings under the same network name.

| CalNet configuration | Observation |
| --- | --- |
| WPA3-Personal on channel 2 | One discovery pass counted 42 beacons advertising the exact SSID. The AP advertised SAE, CCMP and required protected management frames. The station exited during discovery, before association or password access. |
| WPA2-Personal on channel 2 | The dongle found a compatible PSK/CCMP beacon, completed Open System authentication and association, then completed the WPA2 four-way handshake and installed CCMP keys. A DHCP server answered the dongle's encrypted DHCPDISCOVER. The prototype decrypted the offer and matched its transaction ID and client MAC before shutting down the adapter. |

The original `--join` trial ended at the offer stage. Use `--connect` or `--connect-saved` to keep the station running and give macOS an IPv4 connection.

## Internet access on CalNet

On the same M1 Max MacBook Pro, the foreground companion joined CalNet using its saved credential. macOS reported a DHCP ACK on the temporary host interface and verified the router by ARP. With built-in Wi-Fi still on, a GitHub HTTPS request using the dongle's address returned HTTP 200.

With built-in Wi-Fi **off**, macOS listed only the dongle interface as reachable. An ordinary GET of `https://github.com/` used the dongle’s DHCP address, passed TLS certificate verification, returned **HTTP 200**, and downloaded **537,043 bytes**. The request used the normal resolver and route. The VPN interface present before the switch was absent during this transfer. Built-in Wi-Fi was restored, and both temporary interfaces disappeared when the station stopped.

On a second run with **both radios on**, macOS showed distinct IPv4 addresses for built-in Wi-Fi and the dongle on the same CalNet subnet. All three pings bound to the dongle received replies from the home router while an ordinary GitHub request used the active VPN and returned HTTP 200. Control-C exited successfully and released both temporary interfaces. A test with the dongle attached to a separate Stretch router is still pending.

### Repeat the internet test

1. In Terminal, start the station and keep it running.

   ```sh
   station="$(sh build-station.sh)" && "$station" --connect-saved 'CalNet'
   ```

2. Approve the temporary interface in the macOS administrator dialog. Wait for the `Host link ready` message and its IPv4 address. To check the address in a second Terminal, replace `feth0` with the printed interface name.

   ```sh
   /usr/sbin/ipconfig getifaddr feth0
   ```

3. To reproduce the browser scenario, turn built-in Wi-Fi off after DHCP succeeds. Reload GitHub in your browser or run the following in the second Terminal. HTTP 200 and a local address matching the dongle's lease identify the result observed in this trial.

   ```sh
   /usr/bin/curl -q --max-time 15 --silent --show-error --output /dev/null \
     --write-out 'HTTP %{http_code}, local IP %{local_ip}, bytes %{size_download}\n' https://github.com/
   ```

   To have the included checker compare the address for you, run `/path/to/build-station.XXXXXX/host-link-verify feth0 --https` with the actual build directory and interface name. A VPN or proxy may select another address. The checker reports when it cannot establish which interface carried the request.

4. Turn built-in Wi-Fi back on, then press **Control-C** in the station Terminal. The helper releases the dongle’s temporary interfaces.

## Repeat the one-shot radio test

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
