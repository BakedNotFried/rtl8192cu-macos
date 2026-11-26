# Stretch 2.4 GHz Wi-Fi on Apple silicon

Connect a Realtek RTL8192CU USB Wi-Fi dongle to the Stretch router from an Apple silicon Mac. A small Terminal companion handles Wi-Fi and gives macOS a temporary Ethernet connection that your apps can use. The built-in Wi-Fi menu continues to control the Mac’s own radio.

**Tested on an M1 Max MacBook Pro running macOS 27.2.1.** With built-in Wi-Fi off, the dongle loaded GitHub over a separate 2.4 GHz WPA2 network. The Stretch router and other MacBook generations still need a hardware trial. [CalNet test results](docs/testing/README.md)

## What you need

- An Apple silicon Mac with Apple Command Line Tools and a Realtek RTL8192CU dongle with USB ID `0bda:8178`.
- The exact name and password of the Stretch router’s **2.4 GHz** network. It must broadcast its name and offer **WPA2-Personal with AES/CCMP** with protected management frames optional or disabled. The companion uses DHCPv4 to get an address automatically.

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
"$station" --connect-saved "$ssid"
```

The password must be saved on the Mac for the last line. After Wi-Fi authentication, approve the macOS administrator prompt to create the temporary Ethernet connection. This installs no software.

**3. Use both connections.** Leave built-in Wi-Fi on for internet access. When the station prints `Host link ready` and an IPv4 address, open the Stretch service at the address supplied by the robot’s operator. Keep the station running while you use it. This setup works best when Stretch and your internet Wi-Fi use different IP subnets.

Press **Control-C** to disconnect and release the temporary interfaces. To reconnect, run the same connect command again. The older `--join` and `--join-saved` commands run a short radio test and exit.

## Check the connection

The station’s Terminal shows the Wi-Fi network name, channel, and WPA2 status. Look for the line starting with `Host interface:` to find the dongle’s macOS interface name. In a **second Terminal**, set `dongle` to that name. The example uses `feth0`. Check the name again after reconnecting.

```sh
dongle=feth0
ipconfig getsummary "$dongle"
```

The summary shows the Mac’s IPv4 address on Stretch and the settings supplied by the router, including the subnet mask, gateway, and DNS servers. For just the Mac’s address, run `ipconfig getifaddr "$dongle"`.

To check which connection macOS will use for the robot, replace `ROBOT_IP` with the **robot’s IPv4 address**. Look for the `interface:` line in the result. It should match the dongle’s interface name.

```sh
route -n get ROBOT_IP
```

## Change connection settings

**Set a static IPv4 address.** In the second Terminal, replace `RESERVED_IP` with the address the Stretch operator assigned to **your Mac** and `SUBNET_MASK` with their dotted mask (for example, `255.255.255.0`). This replaces DHCP on the dongle for the current session and requires administrator approval. Manual mode sets only the address and mask.

```sh
sudo ipconfig set "$dongle" MANUAL RESERVED_IP SUBNET_MASK
```

**Return to automatic addressing.** Run this in the same Terminal to request a fresh DHCP lease. The companion also starts with DHCP each time you reconnect.

```sh
sudo ipconfig set "$dongle" DHCP
```

**Switch Wi-Fi networks.** In the **original Terminal**, press **Control-C**, then run the command below with the new network’s exact name inside the quotes. Use `--connect-saved` if its password is already saved on this Mac.

```sh
"$station" --connect "New network name"
```

[Personal CalNet trial](docs/testing/README.md) · [How the temporary interface works](host_link/README.md) · [Hardware source](https://github.com/openbsd/src/tree/c402c876540cad83461325f87a703ff2c9c32757) · [Firmware license](radio/upstream/urtwn-license) · [Supplicant credits](security/UPSTREAM.md)
