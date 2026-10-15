# RTL815x DriverKit — Development Plan

## Goal

Build a DriverKit system extension for Realtek RTL8153 (1GbE USB) on Apple Silicon macOS,
using the AQC111Driver project as the architectural template. RTL8159 (10GbE) support follows
once hardware is available; the RTL8153 driver establishes the host-side infrastructure.

---

## Hardware Facts (from lsusb, 2026-05-01)

- VID: `0x0BDA`, PID: `0x8153`
- USB 3.0 SuperSpeed (5Gbps link)
- 2 configurations:
  - **Config 1** — vendor-specific (class 255), 1 interface, 3 endpoints — **target**
  - **Config 2** — CDC ECM, 2 interfaces — currently matched by AppleUserECM → en8

### Config 1 Endpoints

| EP | Address | Type | Direction | wMaxPacketSize | Role |
|----|---------|------|-----------|----------------|------|
| 1  | `0x81`  | Bulk | IN        | 1024B          | RX   |
| 2  | `0x02`  | Bulk | OUT       | 1024B          | TX   |
| 3  | `0x83`  | Interrupt | IN  | 2B             | Link status |

EP3 is a 2-byte interrupt pipe delivering link/status change notifications. Format from
Linux `r8152.c` `r8153_interrupt`. Note: RTL8159 (0x815A) EP3 has `wMaxPacketSize=16B`
(matching AQC111), `wBytesPerInterval=2` — read buffer must be 16B, not 2B. See RTL8159_HW_LOG.md.

---

## Repository Layout

```
RTL815xDriverKit/
├── Common/          — shared headers included by both chip drivers
│   ├── RTLOcp.h     — OCP read/write helpers (PLA/USB MCU, vendor control transfers)
│   ├── RTLDesc.h    — rx_desc / tx_desc structs and frame packing/parsing
│   └── RTLRegs.h    — PLA/USB register address constants
├── RTL8153/         — Xcode project (private; patch blob file gitignored)
│   ├── .gitignore   — excludes RTL8153PatchCode.h (MAC + GPHY firmware arrays)
│   └── ...
├── RTL8159/         — Xcode project (fully public, clean — no firmware blobs)
│   └── ...
├── RE_LOG.md
├── LINUX_LOG.md
├── NDIS_RE_LOG.md
├── NDK_RE_LOG.md    — NetworkingDriverKit binary RE (clone packet API investigation, 2026-05-14)
├── RTL8159_HW_LOG.md — USB descriptor analysis for RTL8159 (0x815A), 2026-05-14
└── PLAN.md
```

**Rationale:** RTL8153 and RTL8159 have different public shareability.
RTL8153 uses firmware patch blobs (`SubmitMACPatchCode`/`SubmitGphyPatchCode` from kext RE;
Linux `r8152_firmware.c`) that are Realtek IP — kept local, never committed publicly.
RTL8159 has no patch code (Linux `r8159_patch_code` is an empty stub) — fully clean.
Separate Xcode projects with shared `Common/` headers avoids duplication while keeping
the public/private boundary hard.

The NDIS per-chip `.sys` layout (one binary per chip) likely has the same shared-source
internal model — we just can't verify from the outside.

---

## Architecture (per chip, identical pattern)

### Dual-Personality DriverKit Extension (mirrors AQC111)

**Personality A — Device (Composite)**
- Matches: `IOUSBHostDevice`, VID=`0x0BDA`, PID=`0x8153` (or `0x815A` for RTL8159)
- IOClass: `IOUserService`, CFBundleIdentifierKernel: `com.apple.iokit.IOUSBHostFamily`
- Action: Open device, `SetConfiguration(1, true)`, keep session open to pin Config 1
- Does NOT close the device — holds exclusive session for the lifetime of the driver

**Personality B — NIC (Interface)**
- Matches: `IOUSBHostInterface`, VID=`0x0BDA`, PID=`0x8153` (or `0x815A`),
  `bConfigurationValue=1`, `bInterfaceNumber=0`, `bInterfaceClass=255`
- IOClass: `IOUserNetworkEthernet`, CFBundleIdentifierKernel: `com.apple.iokit.IOSkywalkFamily`
- Action: full network stack (OCP init, PHY bring-up, RX/TX, link status)

### Bundle IDs (to be registered)

**RTL8153:**
- Loader app: `au.com.jquirke.RTL8153Loader`
- Driver extension: `au.com.jquirke.RTL8153Driver`
- Provisioning profile: `driverkit`, `driverkit.transport.usb`, `driverkit.family.networking`, PID `0x8153`

**RTL8159 (deferred):**
- Loader app: `au.com.jquirke.RTL8159Loader`
- Driver extension: `au.com.jquirke.RTL8159Driver`
- Provisioning profile: same entitlements, PID `0x815A`

Team ID: `R83642DDMF` (both)

---

## Register Access Model

RTL815x uses the **OCP (On-Chip Programming)** model — all chip state accessed via
USB vendor control transfers, not direct MDIO or memory-mapped registers.

Two MCU domains:
- **PLA** (`McuType = 0x0100`): packet/link/MAC registers
- **USB** (`McuType = 0x0000`): USB controller registers

Access format (from kext RE + Linux `r8152.c`):
- `bRequest = 0x05` — OCP read (bmRequestType = `0xC0`)
- `bRequest = 0x05` — OCP write (bmRequestType = `0x40`)
- `wValue` = register address, `wIndex` = length/byte lane

MAC address: read from PLA register `PLA_IDR` (`0xC000`, 6 bytes).

---

## Link Status

Config 1 EP3 IN Interrupt delivers a **2-byte** status word on link state change.
Format from Linux `r8152.c` `r8153_interrupt` / `r8152_istat`:
- Bit 0: link change
- Bit 1+: speed/duplex state

Unlike AQC111 (which only fires on transitions), Realtek re-delivers state on every
interrupt pipe re-arm — so the ECM pattern (arm → get current state) applies directly.
No need to cache last state across `SetInterfaceEnable` calls.

---

## Init Sequence (RTL8153, from Linux `rtl8153_init` + kext RE)

High-level phases mirroring `rtl8153_init()` in `r8152.c`:

1. **USB mode setup** — disable U1/U2, LTM, disable selective suspend during init
2. **Wait for autoload** — poll until firmware autoload completes
3. **PHY power-down clear** — clear BMCR powerdown bit
4. **LAN_ON wait** — poll `PHY_STAT` until `STAT_LAN_ON`
5. **USB queue / power config** — set USB RX aggregation, queue depth
6. **Flow control** — set PLA flow control registers
7. **MAC address read** — read 6 bytes from `PLA_IDR`
8. **Medium setup** — set speed/duplex in `PLA_CR`, enable RX path
9. **Interrupt arm** — post first async read on EP3 (interrupt pipe)
10. **Register Ethernet interface** — `RegisterEthernetInterface`, `RegisterService`

PHY configuration handled via **MDIO-over-OCP** using `r8153_phy_read` / `r8153_phy_write`
(Linux helpers). No separate bRequest for PHY — goes through OCP registers.

Primary reference: `/Users/jeremyquirke/github.com/realtek-r8152-linux/r8152.c`,
functions: `rtl8153_init`, `rtl8153_up`, `rtl8153_down`, `rtl8153_disable`.

---

## TX/RX Packet Format

From kext RE (same for all RTL815x in Config 1):

- **RX aggregation**: multiple frames packed in one USB transfer. Each frame prefixed by
  a descriptor (`rx_desc` from Linux) carrying frame length, checksum result, VLAN tag.
  RX buffer size: 66560 bytes (0x10400) — same as kext `IOBufferMemoryDescriptor` size.
  Zero-copy aggregation is not possible (no public clone packet API — see `NDK_RE_LOG.md`).
  Phase 4a ships non-aggregated (firmware aggregation disabled). Phase 4b re-enables firmware
  aggregation with a memcpy design: parse the rx_desc chain from the USB completion buffer,
  `memcpy` each frame into its own pool slot, deliver N packets per completion. USB-efficient
  (fewer completions, fuller transfers) at the cost of one userspace memcpy per frame.
- **TX aggregation**: multiple frames packed per USB transfer. Each frame prefixed by
  `tx_desc` (Linux struct) carrying frame length and offload hints.

Descriptor layout from Linux `r8152.h`:
```c
struct rx_desc {
    __le32 opts1;   // bits[17:0] = frame length, bit18 = CRC ok, ...
    __le32 opts2;   // VLAN tag
    __le32 opts3;
    __le32 opts4;
};  // 16 bytes

struct tx_desc {
    __le32 opts1;   // bits[15:0] = frame length, checksum offload hints
    __le32 opts2;   // VLAN tag
};  // 8 bytes
```

---

## Milestones

### Phase 1 — Scaffold RTL8153 (no hardware bring-up)
- [ ] Create `RTL8153/` Xcode project (copy AQC111 structure, rename)
- [ ] Create `Common/` with placeholder headers (`RTLOcp.h`, `RTLDesc.h`, `RTLRegs.h`)
- [ ] Two personalities in Info.plist (Device + NIC)
- [ ] Loader app (copy AQC111Loader, update bundle IDs)
- [ ] Device personality: `SetConfiguration(1, true)`, `RegisterService`
- [ ] NIC personality: `Start()` stub — opens interface, copies pipes, registers
      Ethernet interface with placeholder MAC — confirms DriverKit matching works
- [ ] New provisioning profile registered in Apple Developer portal for PID `0x8153`
- [ ] `RTL8153/.gitignore` excludes `RTL8153PatchCode.h`

### Phase 2 — OCP register access + MAC read
- [ ] Implement OCP read/write in `Common/RTLOcp.h` (vendor control transfers, PLA/USB MCU)
- [ ] Read MAC address from `PLA_IDR` at `Start()`
- [ ] Log chip ID / version (`queryHardwareVersion` equivalent)

### Phase 3 — Basic init + link status
- [ ] Implement `rtl8153_init` sequence (USB mode, autoload wait, PHY power-up)
- [ ] Arm interrupt pipe (EP3), implement `OnInterruptComplete` → parse 2-byte status
- [ ] `reportLinkStatus` from interrupt handler
- [ ] `ifconfig en? up` shows `status: inactive` / `status: active`

### Phase 3a — Firmware patches (local only, never committed publicly)
- [ ] RE `AppleRTL8153Controller::SubmitMACPatchCode` and `SubmitGphyPatchCode`
- [ ] Implement equivalent in `RTL8153PatchCode.h` (gitignored)
- [ ] Call from `hardwareInit` — compare link quality / PHY behaviour with/without

### Phase 4a — RX path (non-aggregated)
- [ ] Disable firmware RX aggregation (`USB_RX_AGGR_NUM` / aggregation timeout registers)
- [ ] Post bulk IN reads on EP1 with single-frame buffers; strip leading `rx_desc` header
- [ ] Deliver one `IOUserNetworkPacket` per frame to Skywalk
- [ ] Verify with Wireshark

### Phase 4b — RX aggregation (memcpy design)

Zero-copy is blocked (no public clone packet API — `NDK_RE_LOG.md`). Memcpy design:
- [ ] Re-enable firmware RX aggregation (`USB_RX_AGGR_NUM` / aggregation timeout registers)
- [ ] Implement `rx_desc` chain parser in `Common/RTLDesc.h`: walk the 16-byte descriptors,
      extract frame length from `opts1[17:0]`, advance pointer by `(len + 3) & ~3` (4B align)
- [ ] For each frame: dequeue a pool packet, `memcpy` frame data into it, set length, enqueue
      (Apple's libSystem `memcpy` switches to non-temporal stores above ~4KB; for typical
      Ethernet frames it stays temporal — destination lands in L1/L2 which Skywalk reads
      immediately, so cache warming is net-positive here)
- [ ] Deliver N packets per USB bulk-IN completion to Skywalk
- [ ] Verify throughput improvement over Phase 4a baseline with `iperf3`

### Phase 5 — TX path
- [ ] Implement TX frame packer (`tx_desc` prepend) in `Common/RTLDesc.h`
- [ ] Post bulk OUT on EP2
- [ ] Verify ARP → ping

### Phase 6 — RTL8159 (hardware available, PID=0x815A confirmed)
- [ ] Create `RTL8159/` Xcode project, register provisioning profile for PID `0x815A`
- [ ] Inherit `Common/` headers directly
- [ ] RTL8159-specific init from `r8159_init` / `r8159_hw_phy_cfg` (no patch blobs)
- [ ] 10G media type in `ReportAvailableMediaTypes`

---

## Key Differences from AQC111

| Aspect | AQC111 | RTL8153 |
|--------|--------|---------|
| Link status pipe | EP1 IN Interrupt 16B | EP3 IN Interrupt 2B |
| Interrupt re-delivery | Transition only (must cache) | Re-delivers on every arm |
| Register access | AQ_ACCESS_MAC bRequest=0x01 | OCP bRequest=0x05, PLA/USB MCU |
| PHY access | FWPhyAccess bRequest=0x61 | MDIO-over-OCP |
| TX/RX framing | AQ aggregate format | RTL rx_desc/tx_desc (Linux structs) |
| Init reference | Linux `aqc111.c` + old macOS kext | Linux `r8152.c` (open source, full) |

---

## Open Questions

1. **Interrupt pipe format**: Linux `r8152_istat` is a u16. Confirm bit layout against
   live device once Config 1 is active.
2. **RX aggregation in DriverKit**: Zero-copy aggregated RX is blocked — no public clone
   packet API in NetworkingDriverKit (`NDK_RE_LOG.md`). **Resolution**: Phase 4a ships
   non-aggregated (firmware aggregation disabled). Phase 4b re-enables firmware aggregation
   with a memcpy design — parse rx_desc chain, `memcpy` each frame into a pool slot, deliver
   N packets per completion. USB efficiency preserved; memcpy cost is one user-space copy per
   frame into cache-warm pool memory.
3. **Competing drivers and matching race**:
   - **RTL8153**: `AppleUserECM.dext` matches Config 2 (ECM). Both competitors are dexts —
     symmetric startup latency. Once Personality A pins Config 1, ECM loses its interface.
     Confirm teardown is graceful (no panic).
   - **RTL8159**: `AppleUSBNCM.kext` matches Config 2 (NCM). **Asymmetric race** — kext has
     near-zero start latency; our Personality A dext must launch a user-space process before
     `Start()` is called. If the USB stack publishes Config 2 interface nubs before our dext
     starts, NCM can attach faster than we can call `SetConfiguration(1, true)`.
   - **Mitigation**: set a high `IOProbeScore` on Personality A; keep `IOMatchCategory` tight.
     `SetConfiguration` should still invalidate NCM's interface nubs even if it has already
     probed — verify this is graceful on the bench device.
   - **Experiment**: plug in RTL8159 with driver installed; check `ioreg`/Console for evidence
     of NCM attaching before Personality A completes. A definitive test is whether
     `bConfigurationValue` in ioreg shows 2 (NCM won) or 1 (we won) at steady state.
   - **Last resort**: if the race proves non-deterministic or NCM reliably wins, investigate
     development-only ways to suppress or out-prioritize `AppleUSBNCM.kext` on the test
     machine, such as boot-arg / `kmutil` approaches, if supported on the target OS.
     Document as dev-only workaround — shipping driver must win the race cleanly.
4. **RTL8159 PID**: Confirmed `0x815A` (ioreg, 2026-05-14). Hardware available — Phase 6 unblocked.
5. **Firmware patches necessity**: RE `SubmitMACPatchCode`/`SubmitGphyPatchCode` call sites
   in `hardwareInit` to understand whether they are skippable for basic operation, or
   required for PHY bring-up. AppleUserECM evidence (CDC, no patches, device works) suggests
   skippable — validate once Config 1 driver is running.
6. **Clone packet API**: **CLOSED — no public path.** `IOUserNetworkPacketState` is entirely
   `#if __NDK_PRIVATE__`. `_AllocatePacket` is a kernel IORPC — packet slots are allocated
   kernel-side, not locally. `isClonePacket:1` changes dealloc semantics (clone packets lack
   a direct kernel proxy and go through an IORPC `_DeallocatePacket` path with `.clone.cold`
   error handlers), but clone creation is kernel-gated; a dext cannot request it. Phase 4b
   permanently closed. Full analysis in `NDK_RE_LOG.md`.
