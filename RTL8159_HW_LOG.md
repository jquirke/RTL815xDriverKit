# RTL8159 (0x815A) Hardware Log — USB Descriptors

Captured 2026-05-14 via `lsusb -v -d 0bda:815a` on Apple Silicon macOS 15.
Device purchased from Alibaba.

---

## Device Descriptor

| Field               | Value                              | Notes                                    |
|---------------------|------------------------------------|------------------------------------------|
| VID                 | `0x0BDA`                           | Realtek                                  |
| PID                 | `0x815A`                           | RTL8159 / RTL815A                        |
| bcdUSB              | 3.20                               | USB 3.2 Gen 2 — **10 Gbps**, not 5Gbps  |
| bcdDevice           | 30.00                              | Firmware version 30                      |
| bDeviceClass        | 0                                  | Per-interface class                      |
| bMaxPacketSize0     | 9                                  | 2⁹ = 512B control EP (USB3 exponent)    |
| bNumConfigurations  | **3**                              | RTL8153 has 2 — see below               |
| iManufacturer       | 1                                  | (Realtek)                                |
| iProduct            | 2                                  | "USB 10/100/1G/2.5G/5G/10G LAN"        |
| iSerial             | 7                                  |                                          |
| MaxPower (all cfgs) | 544 mA                             |                                          |

---

## Configurations Overview

| Config | bConfigurationValue | Description        | Competing macOS driver          |
|--------|---------------------|--------------------|---------------------------------|
| 1      | 1                   | Vendor-specific    | None (our target)               |
| 2      | 2                   | CDC NCM            | `com.apple.driver.usb.cdc.ncm`  |
| 3      | 3                   | CDC ECM            | `AppleUserECM`                  |

**`kUSBPreferredConfiguration = 2`** (ioreg) — macOS defaults to CDC NCM.
Personality A must call `SetConfiguration(1, true)` to override.

RTL8153 (0x8153) had only 2 configurations: vendor + CDC ECM.
RTL8159 adds a third (CDC NCM) and promotes it to the preferred slot.

---

## Config 1 — Vendor-Specific (Driver Target)

- `bConfigurationValue = 1`
- `bNumInterfaces = 1` — single interface, `bInterfaceNumber = 0`
- `bInterfaceClass = 255`, `bInterfaceSubClass = 255`, `bInterfaceProtocol = 0`
- **4 alternate settings** (0–3) — all expose the same 3 endpoint addresses,
  differing only in `bMaxBurst` and `MaxStreams` on the bulk EPs.

### Alternate Setting Comparison

| AltSetting | EP1 IN bMaxBurst | EP1 MaxStreams | EP2 OUT bMaxBurst | EP2 MaxStreams |
|------------|:----------------:|:-------------:|:-----------------:|:-------------:|
| 0          | 6                | —             | 6                 | —             |
| 1          | 7                | 8             | 6                 | —             |
| 2          | 7                | 4             | 7                 | —             |
| 3          | 7                | 2             | 7                 | —             |

AltSetting 0 is the simplest (no streams). Linux `r8159.c` likely selects a specific
alternate setting during init — confirm from source before choosing.

### Config 1 Endpoints (common across all alt settings)

| EP  | Address | Type      | Dir | wMaxPacketSize | bInterval | bMaxBurst | wBytesPerInterval |
|-----|---------|-----------|-----|:--------------:|:---------:|:---------:|:-----------------:|
| 1   | `0x81`  | Bulk      | IN  | 1024B          | 0         | varies    | —                 |
| 2   | `0x02`  | Bulk      | OUT | 1024B          | 0         | varies    | —                 |
| 3   | `0x83`  | Interrupt | IN  | **16B**        | 11        | 0         | **2**             |

**EP3 interrupt pipe**: `wMaxPacketSize = 16B`, `wBytesPerInterval = 2`.
The device commits only 2 bytes of payload per interval, but the USB packet
capacity is 16 bytes — allocate a **16-byte read buffer** in the driver
(`AsyncIO` completion will deliver 2 meaningful bytes). This matches AQC111's
EP1 Interrupt 16B in wMaxPacketSize, unlike RTL8153 whose EP3 was 2B wMaxPacketSize.

---

## Config 2 — CDC NCM (Preferred by macOS)

- `bConfigurationValue = 2`, 2 interfaces
- **Interface 0** (Communications Control):
  - `bInterfaceClass = 2`, `bInterfaceSubClass = 13` → **CDC NCM** (not ECM)
  - CDC Ethernet descriptor: `iMacAddress = 3` (hardware address omitted)
  - `wMaxSegmentSize = 1518`, `wNumberMCFilters = 0x8000`
  - CDC NCM descriptor: `bcdNcmVersion = 1.00`, `bmNetworkCapabilities = 0x2b`
  - EP 3 IN Interrupt: `wMaxPacketSize = 16B`, `wBytesPerInterval = 8`
- **Interface 1, AltSetting 0** (Data, zero endpoints — inactive)
  - `bInterfaceClass = 10`, `bInterfaceSubClass = 0`, `bInterfaceProtocol = 1`
- **Interface 1, AltSetting 1** (Data, active):
  - EP 1 IN Bulk 1024B `bMaxBurst = 6`
  - EP 2 OUT Bulk 1024B `bMaxBurst = 6`

---

## Config 3 — CDC ECM

- `bConfigurationValue = 3`, 2 interfaces
- **Interface 0** (Communications Control):
  - `bInterfaceClass = 2`, `bInterfaceSubClass = 6` → **CDC ECM**
  - CDC Ethernet descriptor: same hardware address as configuration 2
  - EP 3 IN Interrupt: `wMaxPacketSize = 16B`, `wBytesPerInterval = 8`
- **Interface 1, AltSetting 0** (Data, zero endpoints — inactive)
  - `bInterfaceClass = 10`, `bInterfaceSubClass = 0`, `bInterfaceProtocol = 0`
- **Interface 1, AltSetting 1** (Data, active):
  - EP 1 IN Bulk 1024B `bMaxBurst = 6`
  - EP 2 OUT Bulk 1024B `bMaxBurst = 6`

---

## BOS / Device Capabilities

| Capability          | Detail                                                  |
|---------------------|---------------------------------------------------------|
| USB 2.0 Extension   | BESL LPM Supported                                      |
| SuperSpeed          | LTM Supported; Full/High/SuperSpeed (5G) speeds         |
| SuperSpeedPlus      | 10 Gbps symmetric RX and TX (SubLink Speed ID 0)        |
| bFunctionalitySupport | 2 → minimum fully-functional speed is High Speed (480 Mbps) |
| bU1DevExitLat       | 10 µs                                                   |
| bU2DevExitLat       | 2047 µs                                                 |

LTM is supported — disable it during init (same as `rtl8153_init` U1/U2/LTM disable sequence).

---

## MAC Address

The hardware address is available from CDC Ethernet descriptor string index 3.
An 8-byte OCP read of `PLA_BACKUP` (`0xD000`) also returned it with configuration 1 active.

---

## Key Differences vs RTL8153 (0x8153)

| Aspect                   | RTL8153                         | RTL8159 (0x815A)                         |
|--------------------------|---------------------------------|------------------------------------------|
| USB version              | USB 3.0 (5 Gbps)                | USB 3.2 Gen 2 (10 Gbps)                 |
| bNumConfigurations       | 2                               | **3**                                    |
| CDC preferred config     | Config 2 = ECM                  | Config 2 = **NCM**, Config 3 = ECM      |
| Competing macOS driver   | AppleUserECM (Config 2)         | `cdc.ncm` (Config 2), AppleUserECM (Config 3) |
| Config 1 alt settings    | 1 (alt 0 only)                  | **4** (alt 0–3, varying MaxBurst/Streams) |
| EP3 wMaxPacketSize       | 2B                              | **16B** (wBytesPerInterval still 2)      |
| EP3 read buffer size     | 2B                              | **16B**                                  |

---

## Driver Notes for RTL8159 Xcode Project

1. **Info.plist matching**: `idProduct = 33114` (`0x815A`), `bConfigurationValue = 1`,
   `bInterfaceClass = 255`, `bInterfaceNumber = 0`.
2. **Personality A**: `SetConfiguration(1, true)` displaces both NCM (Config 2) and
   ECM (Config 3) drivers simultaneously.
3. **Alternate setting**: Start with AltSetting 0 (no streams, bMaxBurst=6/6).
   Check Linux `r8159.c` `SetInterface` call to confirm which alt setting it selects.
4. **EP3 buffer**: Allocate 16 bytes (wMaxPacketSize), not 2. Only bytes [0:1] are
   meaningful — format per Linux `r8152_istat` / `r8153_interrupt`.
5. **Init sequence**: `r8159_init` in Linux `r8152.c` — not `rtl8153_init`.
   Disable U1/U2/LTM at start (BOS confirms LTM Supported, device will accept the disable).
