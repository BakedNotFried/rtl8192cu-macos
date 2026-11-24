# RTL8192CU USB Wi-Fi for Apple silicon

A macOS command-line prototype for the Realtek **RTL8192CU** USB adapter, device ID **`0bda:8178`**. Use it to identify the chip, receive 2.4 GHz beacons, and test whether the dongle can join a WPA2-Personal network.

**Current status:** the dongle booted its firmware and received real beacons on an M1 Max MacBook Pro running macOS 27.2.1. The WPA2 four-way handshake passed offline against a hostap authenticator. **Joining a physical WPA2 access point and exchanging encrypted traffic are still unverified.** Other MacBook generations also await hardware testing.

Each command runs in the foreground, uses the dongle for one test, then shuts it down. Ordinary Mac apps continue using their existing network connection. A persistent network interface and companion app are future work.

## What you need

- An Apple silicon Mac with the Apple Command Line Tools and macOS SDK. Check with `xcrun --find clang` and `xcrun --show-sdk-path`.
- One connected USB adapter matching `0bda:8178`. The radio code targets the production RTL8192C two-chain variant. The label “802.11n WLAN Adapter” alone does not identify it.
- For the join test, an access point you control that broadcasts its name on **2.4 GHz** and offers **WPA2-Personal (PSK) with AES/CCMP**. Protected management frames (PMF) may be optional or disabled. The prototype cannot join WPA3-only, PMF-required, enterprise or 5 GHz-only networks.
- For the encrypted traffic check, the test network needs a DHCPv4 server.

The probe and station use Apple's existing USB APIs. Builds run locally without installing a driver extension, changing network settings, or downloading dependencies. Run the commands as your regular user.

## First run

**1. Get the source.** With a GitHub account that has access to this private repository:

```sh
gh repo clone BakedNotFried/rtl8192cu-macos
cd rtl8192cu-macos
```

**2. Check the adapter.** This reads its USB descriptors and chip registers. Look for `Matched USB 0bda:8178` followed by the RTL8192CU identification.

```sh
probe="$(sh build-probe.sh)" && "$probe"
```

**3. Build the station tool and check your network.** Replace the example name with the exact, case-sensitive SSID. Keep these shell variables in the same Terminal window for the following commands.

```sh
ssid='My WPA2 test network'
station="$(sh build-calnet.sh)" && "$station" --discover "$ssid"
```

Discovery listens for beacons on channels 1–13. A compatible result includes `compatible 2.4 GHz WPA2-Personal/CCMP beacon` and the channel. Seeing the exact name alone confirms reception. The advertised security determines whether the join test can proceed.

**4. Try a join.** Use **one** of these credential methods after discovery finds a compatible AP.

| Where the password is | Command |
| --- | --- |
| Enter it in a local secure macOS dialog | `"$station" --join "$ssid"` |
| Use the saved macOS Wi-Fi password for that exact SSID | `"$station" --join-saved "$ssid"` |

macOS may prompt you to allow Keychain access for the saved-password method. Neither command takes the password as an argument. To test association alone without requesting a password, run `"$station" --associate "$ssid"`.

## How to read the join result

| Milestone in Terminal | What it establishes |
| --- | --- |
| `Open System authentication accepted` | The access point answered the initial 802.11 management exchange. Password verification comes later. |
| `Association accepted` | The AP assigned an association ID to the dongle. |
| `WPA2 four-way handshake complete; CCMP keys installed` | The supplicant completed the key exchange and the prototype installed the session keys. |
| `Encrypted bidirectional data verified by a matching DHCP offer` | A DHCP server answered the dongle's encrypted discovery packet. The prototype matched the transaction and client address. This is the end-to-end success criterion. |

The test stops after the offer, before requesting a lease. It leaves the Mac's routing table alone. Press **Control-C** to end a test early and let the process run its device cleanup.

## Other checks

To check physical reception without choosing a network, build the passive receiver. With no arguments it stops at the first verified infrastructure beacon. The example below requests channel 6.

```sh
receiver="$(sh build-radio.sh)" && "$receiver" 6
```

To run the offline test suite, including firmware transfer, station frame handling, WPA2 key exchange and DHCP parsing:

```sh
sh test.sh
```

Each build uses a new directory inside the checkout and prints the executable path. You can reuse that binary in later sessions. Build directories are ignored by Git.

## Troubleshooting

| Observation | What to check |
| --- | --- |
| Probe expects exactly one matching adapter | Connect one matching dongle and rerun the probe. Check the USB vendor/product ID if it still fails. |
| USB user-client or interface-open error | Run the binary in a local Terminal with ordinary USB access. Close another test process if it holds the dongle. |
| Discovery hears the SSID but finds no compatible AP | Read the reported security fields. For example, `akm=SAE` with mandatory PMF identifies a WPA3-only AP if no PSK method is advertised. Use a separate compatible test network. |
| Password dialog or Keychain lookup fails | Check the exact SSID. For a network whose password is not saved, use `--join` in a logged-in graphical session. |
| Handshake completes but the DHCP proof times out | Confirm DHCPv4 is available on the test network. Record the last successful milestone when reporting the result. |
| RF chain B reports an unverified channel | This occurred during successful physical beacon tests. Chain A echoed the requested channel. Independent operation of chain B remains unverified. |

## Hardware evidence and source credits

Two consecutive tests on the M1 Max MacBook Pro (`MacBookPro18,2`) loaded firmware into volatile device memory and received a broadcast beacon. The adapter was shut down and reopened between the tests, with channels 1 and 6 requested. A later live scan identified a WPA3-only access point and exited before credential access. The offline WPA2 tests completed all four handshake messages, with matching pairwise and group keys at both simulated peers.

The hardware implementation uses [OpenBSD rtwn/urtwn](https://github.com/openbsd/src/tree/c402c876540cad83461325f87a703ff2c9c32757) and the [Realtek firmware license](radio/upstream/urtwn-license). The repository includes the hostap sources needed to build and test the supplicant. See [supplicant provenance and licensing](security/UPSTREAM.md) and the [DriverKit distribution investigation](DISTRIBUTION.md).
