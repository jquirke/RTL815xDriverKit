# RTL815xDriverKit

Experimental macOS DriverKit USB Ethernet drivers for Realtek RTL815x adapters. The current bring-up target is RTL8159 (`0x0bda:0x815a`), a USB 3.2 Gen 2 10 Gbps adapter. An RTL8153 project is also present, but this README's runtime status describes **RTL8159 only**.

This is an early Skywalk (`IOUserNetworkEthernet`) driver skeleton. It loads and registers an Ethernet interface, but it does not yet provide a working Ethernet link or packet path.

This project is derived from the author's earlier [AQC111Driver](https://github.com/jquirke/AQC111Driver), which is much further along: it has a working Ethernet data path and more complete DriverKit/Skywalk implementation and test notes. Start there if you are looking for a functional driver reference; this RTL8159 port is still in bring-up.

## Hardware and USB configurations

The tested RTL8159 reports three configurations. See [RTL8159_HW_LOG.md](RTL8159_HW_LOG.md) for the captured descriptors and endpoint details.

| Configuration | Interface | Role |
| --- | --- | --- |
| 1 | Vendor-specific (`0xff`), interface 0 | DriverKit target |
| 2 | CDC NCM | Device's reported preferred configuration |
| 3 | CDC ECM | Additional standard Ethernet mode |

Configuration 1 has four alternate settings. The current skeleton uses alternate setting 0, whose endpoints are bulk IN `0x81`, bulk OUT `0x02`, and interrupt IN `0x83`. The interrupt endpoint has a 16-byte maximum packet size; observed completions carry 2 bytes.

## Driver architecture

The RTL8159 dext contains two IOKit personalities in [Info.plist](RTL8159Driver/RTL8159Driver/Info.plist):

1. **`RTL8159-Device`** matches `IOUSBHostDevice` with VID `0x0bda` and PID `0x815a`. `RTL8159Driver` opens the device, calls `SetConfiguration(1, true)`, and holds the device session open. On teardown it calls `SetConfiguration(2, true)` before closing to restore NCM matching.
2. **`RTL8159-NIC`** matches `IOUSBHostInterface` with the same VID/PID, configuration 1, class `0xff`, and interface number 0. `RTL8159NIC` opens the interface, creates the USB pipes and Skywalk queues, and calls `RegisterEthernetInterface`.

The NIC uses `CopyDevice()` for a device reference; the device personality owns the exclusive device session. This follows the two-personality design used by [AQC111Driver](../AQC111Driver/README.md).

## Current status (2026-10-16)

**Observed on hardware:**

- The signed dext activates through the RTL8159 loader app and matches the adapter.
- The device personality selects configuration 1. IORegistry reports `kUSBCurrentConfiguration = 1`.
- The NIC personality matches interface 0, alternate setting 0. `CopyPipe` succeeds for `0x81`, `0x02`, and `0x83`.
- Skywalk registers `en14` in the observed run, with placeholder MAC `02:8a:15:90:00:01`. Interface numbers are assigned by macOS and can change.
- Ten RX transfers and one interrupt transfer are posted. The interrupt callback receives successful 2-byte completions, repeatedly reporting status word `0x0000`.
- After the diagnostic build loaded, the observed system had two RTL8159 dext processes, one for each personality, rather than a growing set of processes.
- In two teardown runs with the explicit configuration-2 handoff and borrowed-provider ownership fix, loader uninstall returned the still-plugged device to NCM and restored `en14` without a replug.

**Not yet implemented or validated:**

- Hardware initialization and PHY/link-state reads are stubs; `ifconfig` reports `media: autoselect (none)`.
- RX aggregate parsing and delivery to Skywalk are incomplete. TX has only a skeleton path. No successful Ethernet traffic has been demonstrated.
- The hardware MAC read is temporarily bypassed for diagnosis; the NIC uses the locally administered placeholder address above.
- The two bytes returned on the interrupt pipe have been observed, but link transitions have not been validated.

**Open issues:**

- With the initial MAC OCP read enabled, the USB port repeatedly powered off about 45 ms after configuration 1 was selected. The read returned `kIOReturnAborted` (`0xe00002eb`); subsequent `CopyPipe` returned a general error (`0xe00002bc`). Bypassing that first read allowed the NIC to start and remain enumerated. This isolates the issue to the read or its timing; the precise hardware/USB cause is unresolved.
- After deactivation through the loader, the dext and its processes disappeared. The adapter was temporarily absent from IORegistry and `lsusb` during early reenumeration attempts. A later reenumeration with the dext still deactivated selected configuration 2: Apple's `com.apple.driver.usb.cdc.ncm` claimed the adapter and created `en14` with the hardware MAC. The cause of the transient absence and earlier corpse accumulation remains unresolved; no reboot was needed to reach the NCM state.
- The explicit configuration-2 handoff returned the still-plugged adapter to NCM. An initial unload still produced the preexisting `IOService::Stop_Impl` crash and corpse messages. Removing releases of the borrowed device and interface providers eliminated those symptoms in two consecutive unload tests. A forced NIC interface close and a `sysextd` termination warning still appeared.
- Per-completion interrupt logging is currently very noisy.

The chronological evidence and limits of each observation are in [TESTING.md](TESTING.md).

## Build and install

1. Open [RTL815xDriverKit.xcworkspace](RTL815xDriverKit.xcworkspace) or `RTL8159Driver/RTL8159Driver.xcodeproj` in Xcode.
2. Select the `RTL8159Loader` scheme and build/run the signed app. The loader embeds the dext.
3. Use **Install** in the loader to submit the activation request. Approve the driver extension in System Settings if macOS asks.
4. Use **Uninstall** in the same app to submit a deactivation request.

The dext requires DriverKit, USB transport, and networking family entitlements. Its [entitlements file](RTL8159Driver/RTL8159Driver/RTL8159Driver.entitlements) and signing configuration must match the provisioning profile. A `CODE_SIGNING_ALLOWED=NO` command-line build confirms compilation but cannot be installed as a system extension.

Extension activation alone does not prove hardware attachment. Check IORegistry and the NIC logs as well. The configuration-2 handoff returned the still-plugged device to NCM in two tests; the earlier delay and failed enumeration attempts remain unexplained.

## Diagnostics

DriverKit `os_log` messages appear under the `kernel` process. Start a stream before activating the dext or reconnecting the adapter:

```sh
log stream --predicate 'process == "kernel" AND eventMessage contains "RTL81"' --level debug
```

The `RTL8159-A` and `RTL8159-NIC` prefixes include compile timestamps, useful for distinguishing an old installed binary from a new build. To suppress the current per-interrupt spam while watching startup:

```sh
log stream --predicate 'process == "kernel" AND eventMessage contains "RTL81" AND NOT eventMessage contains "OnItrComplete"' --level debug
```

Useful read-only checks:

```sh
systemextensionsctl list
lsusb -d 0bda:815a
ioreg -p IOUSB -l -w0 | rg 'kUSBCurrentConfiguration|kUSBPreferredConfiguration|idVendor|idProduct'
ifconfig -a
```

The USB configuration check should be interpreted alongside the matching VID/PID, since other USB devices appear in `ioreg` output. An `en` interface can be identified by its IORegistry parent `RTL8159NIC` or by the current diagnostic placeholder MAC; its number alone does not identify its driver.

See [TESTING.md](TESTING.md) for the bring-up timeline and the controlled tests still needed.
