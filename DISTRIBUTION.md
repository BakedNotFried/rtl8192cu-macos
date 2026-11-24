# RTL8192CU on Apple silicon

## Local build environment

The Mac reports arm64, macOS 27.2.1 (26B131). Its selected toolchain is `/Library/Developer/CommandLineTools`. On 25 November 2026, `xcrun --sdk macosx --show-sdk-version` returned 27.2. The selected toolchain could not resolve `xcrun --sdk driverkit --show-sdk-path`. The macOS SDK is enough for a standalone IOKit probe. Building a driver extension requires an Xcode toolchain with the DriverKit SDK, which can run on a separate build Mac.

## Proposed driver and app

Match the Realtek USB interface using vendor ID `0x0bda` and product ID `0x8178`. The extension would use USBDriverKit to control the device and NetworkingDriverKit to present an Ethernet-style interface to macOS. A companion app would handle Wi-Fi network selection. That architecture still needs a hardware test. [USBDriverKit](https://developer.apple.com/documentation/usbdriverkit) · [IOUserNetworkEthernet](https://developer.apple.com/documentation/networkingdriverkit/iousernetworkethernet)

| Component | Capabilities for the proposed design |
| --- | --- |
| Driver extension | `com.apple.developer.driverkit`, `com.apple.developer.driverkit.transport.usb` allowing the target USB device, and `com.apple.developer.driverkit.family.networking` |
| Companion app | `com.apple.developer.system-extension.install` for activation and `com.apple.developer.driverkit.userclient-access` scoped to the extension’s bundle ID for its control channel |

## Development and release

1. **Local probe.** A command-line executable can read published device properties from the I/O Registry without activating a driver extension. Reading registers over the USB interface is a separate hardware-access test. A successful probe establishes only the operations it actually completed.
2. **Development build.** Apple offers DriverKit family capabilities for development. A signed on-device extension test needs an eligible App ID, the relevant capabilities and a matching development profile. Confirm that the profile covers the required USB transport. The team’s local signing identities, profiles and account privileges have not been inspected. [Development capabilities](https://developer.apple.com/videos/play/wwdc2022/110373/) · [Development provisioning](https://developer.apple.com/help/account/provisioning-profiles/create-a-driverkit-development-provisioning-profile/)
3. **Distribution decision.** The account holder should check eligibility for the complete entitlement set and specify that Realtek owns the VID. Request support for this product ID and describe its companion app. Apple decides the scope of the grant. The public instructions do not establish that this team qualifies for Realtek’s VID. Confirm the approved scope in the distribution profile before committing to a team rollout. [Apple’s entitlement process](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development) · [USB entitlement fields](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.driverkit.transport.usb)
4. **Direct team release.** Embed the driver in the companion app, sign with the team’s Developer ID and appropriate provisioning profiles, and notarize the app. On a test Mac with standard security enabled, activate the extension through the app, approve it when prompted, and verify actual traffic over the dongle. Repeat on the team’s target macOS versions and M-series models before marking them supported. [Extension activation](https://developer.apple.com/documentation/systemextensions/installing-system-extensions-and-drivers) · [Notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
