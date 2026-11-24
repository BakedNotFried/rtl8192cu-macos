# Stretch 2.4 GHz Wi-Fi on Apple silicon

Connect a Realtek RTL8192CU USB Wi-Fi dongle to the Stretch router from an Apple silicon Mac. A small Terminal companion handles Wi-Fi and gives macOS a temporary Ethernet connection that your apps can use. The built-in Wi-Fi menu continues to control the Mac’s own radio.

**Tested on an M1 Max MacBook Pro running macOS 27.2.1.** With built-in Wi-Fi off, the dongle loaded GitHub over a separate 2.4 GHz WPA2 network. The Stretch router and other MacBook generations still need a hardware trial. [CalNet test results](docs/testing/README.md)

## What you need

- An Apple silicon Mac with Apple Command Line Tools and a Realtek RTL8192CU dongle with USB ID `0bda:8178`.
- The exact name and password of the Stretch router’s **2.4 GHz** network. It must broadcast its name, offer **WPA2-Personal with AES/CCMP**, allow optional or disabled protected management frames, and provide DHCPv4.

## Connect to Stretch

**1. Get the source and build.** Run these commands as your regular macOS user. The build stays inside the checkout and uses the macOS SDK.

```sh
gh repo clone BakedNotFried/rtl8192cu-macos
cd rtl8192cu-macos
station="$(sh build-station.sh)"
```

**2. Plug in the dongle and connect.** In the same Terminal, enter the exact 2.4 GHz network name when prompted. A secure macOS dialog will ask for the Wi-Fi password.

```sh
printf 'Stretch 2.4 GHz network name: '
IFS= read -r ssid
"$station" --connect "$ssid"
```

If the password is saved on this Mac, use `"$station" --connect-saved "$ssid"` for the last line. After Wi-Fi authentication, approve the macOS administrator prompt to create the temporary Ethernet connection. This installs no software.

**3. Use both connections.** Leave built-in Wi-Fi on for internet access. When the station prints `Host link ready` and an IPv4 address, open the Stretch service at the address supplied by the robot’s operator. Keep the station running while you use it.

macOS chooses a connection for each destination. This setup works best when Stretch and your internet Wi-Fi use different IP subnets. To check the path to the robot, run `/sbin/route -n get ROBOT_IP` with its actual IPv4 address. The `interface` line should match the `feth` name printed by the station.

Press **Control-C** to disconnect and release the temporary interfaces. To reconnect, run the same connect command again. The older `--join` and `--join-saved` commands run a short radio test and exit.

[Personal CalNet trial](docs/testing/README.md) · [How the temporary interface works](host_link/README.md) · [Hardware source](https://github.com/openbsd/src/tree/c402c876540cad83461325f87a703ff2c9c32757) · [Firmware license](radio/upstream/urtwn-license) · [Supplicant credits](security/UPSTREAM.md)
