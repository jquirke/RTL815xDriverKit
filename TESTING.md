# RTL8159 Testing Log

This file records what was actually observed on hardware, the build under test, and what each test proves. Design work and descriptor analysis live in [PLAN.md](PLAN.md) and [RTL8159_HW_LOG.md](RTL8159_HW_LOG.md). The runtime observations below are from 2026-10-16 unless otherwise noted.

## Test setup

- Device: Realtek `0x0bda:0x815a`, product string `USB 10/100/1G/2.5G/5G/10G LAN`; the earlier descriptor capture identifies it as a USB 3.2 Gen 2 device with three configurations.
- Host: Apple Silicon macOS. The descriptor capture in `RTL8159_HW_LOG.md` was made on macOS 15; the exact OS release of this bring-up run was not recorded here.
- Dext: `au.com.jquirke.RTL8159Driver`, installed and removed with the `RTL8159Loader` app. The bundle contains separate device and NIC personalities.
- Observation tools: `log stream`, `log show` for USB host events, `systemextensionsctl`, `ioreg`, `ifconfig`, `ps`, and `lsusb`.

The timestamp embedded in each driver log line is the **compile time**, not the time the line was emitted. The initial installed NIC binary logged `[Oct 16 2026 06:57:05]`; the diagnostic NIC binary logged `[Oct 16 2026 07:15:15]`.

## Build and activation

| Check | Observation | Conclusion |
| --- | --- | --- |
| Xcode project build | RTL8159 loader/dext built with signing disabled from the command line; a signed Xcode build was installed through the loader. | Project compiles and the signed extension can be activated. |
| `systemextensionsctl list` | `au.com.jquirke.RTL8159Driver` was `activated enabled`. | Extension registration succeeded; this alone does not prove USB matching. |
| Device personality | Log showed `SetConfiguration(1) -> success` and `RegisterService -> 0x0`. | The dext matched `0x0bda:0x815a` and selected vendor configuration 1. |
| NIC personality | Log showed interface 0/class 255/alternate 0 with endpoints `0x81`, `0x02`, `0x83`. | The configuration-1 interface matched the NIC personality. |

## Initial startup failure with the OCP MAC read enabled

The initial build attempted `rtlReadMACAddress()` immediately after opening the interface. The same sequence recurred on each reenumeration:

1. USB host enumerated `0x0bda/815a` at 10 Gbps.
2. `RTL8159Driver` selected configuration 1 successfully.
3. About 45 ms later, `usb-drd2-port-ss@02200000` logged `cableChangeOccurred: powering off` and destroyed the device because the port was powering off.
4. The NIC's MAC read returned `0xe00002eb` (`kIOReturnAborted`). Its subsequent `CopyPipe(0x81)` returned `0xe00002bc` (`kIOReturnError`) with a null pipe.
5. The NIC start failed; the device personality stopped. The device then reenumerated and repeated the sequence.

The port power-off precedes the two reported API failures. Those return codes therefore do not establish that `CopyPipe` caused the reset. The early vendor control request is a candidate trigger, but the log does not record its exact submission time or prove causality. A large number of orphaned dext processes accumulated during the repeated starts; 152 RTL8159 processes were counted before they were cleared.

## Diagnostic build: bypass first OCP request

The diagnostic change in `RTL8159NIC.cpp` skips the early MAC OCP read and assigns placeholder MAC `02:8a:15:90:00:01`. No changes were made to the two-personality matching, configuration selection, interface opening, or pipe addresses. The modified project built successfully with `CODE_SIGNING_ALLOWED=NO`, then a signed Xcode build was installed through the loader.

Observed from the NIC binary compiled at `07:15:15`:

| Check | Observation |
| --- | --- |
| Interface descriptor | Interface 0, class 255, alternate setting 0, three endpoints. |
| Pipes | `CopyPipe` returned `0x0` for RX `0x81`, TX `0x02`, and interrupt `0x83`. |
| Skywalk | `RegisterEthernetInterface OK`; IORegistry showed `RTL8159NIC` as an active `IOUserNetworkEthernet` service and `en14` below it. |
| Queues/transfers | Ten RX transfers and one 16-byte interrupt transfer were posted; `RegisterService` returned `0x0`. |
| Interrupt callback | Repeated completions returned `status=0x0 bytes=2`, with decoded `istat=0x0000`. |
| Interface identity | `ifconfig en14` showed placeholder MAC `02:8a:15:90:00:01` and `media: autoselect (none)`. |
| USB state | IORegistry reported `kUSBCurrentConfiguration = 1`. |
| Process count | Two RTL8159 dext processes were observed after the successful start, consistent with the two personalities; no new process buildup was seen in that check. |

This proves the skeleton can match, open its pipes, register Skywalk Ethernet, and receive interrupt callbacks on this hardware. It does **not** prove PHY link, RX packet delivery, TX packet transmission, or working network connectivity. `hwInit`, link-state query, and RX aggregate parsing are still stubs.

The difference between the two builds strongly implicates the early OCP MAC read or its timing in the disconnect loop. It does not yet show whether the request encoding is wrong, the device needs a settling delay after configuration selection, or some other interaction is involved.

## Deactivation and NCM fallback: unresolved

The loader's **Uninstall** button submitted a system extension deactivation request. Afterward:

- `systemextensionsctl list` showed only the unrelated AQC111 extension.
- `en14` disappeared and no RTL8159 dext processes remained.
- The still-visible USB device initially reported current configuration 1 and preferred configuration 2. NCM had not attached at that point.
- After physical unplug/replug, the USB port logged power-off and power-on transitions but no completed `0x0bda/815a` enumeration. The device was absent from IORegistry and `lsusb`; no NCM interface appeared.
- Reactivating the RTL8159 extension and reenumerating restored the adapter in configuration 1 with `RTL8159NIC` and `en14`. Two RTL8159 processes were present in that check.

This run cannot establish whether macOS would choose NCM on a clean enumeration without the dext, because the device did not enumerate while the dext was deactivated. The temporary absence might involve system state, device state, or teardown behavior. Do not treat this as a confirmed NCM-driver failure or a confirmed RTL8159 teardown bug.

## Next controlled checks

1. Restore hardware MAC reading only after isolating the first vendor request. Compare a no-request build with a build that waits after `SetConfiguration(1)` before the read, and log request start/completion before interpreting a reset.
2. Capture whether the adapter enumerates in configuration 2 on a clean host/port with the RTL8159 dext deactivated. Confirm the matched IORegistry personality, not just the presence or absence of an `en` number.
3. Implement and validate hardware initialization, real MAC reading, link state, RX aggregate parsing, and TX framing incrementally. Require on-wire evidence before marking Ethernet traffic as working.

For live dext logs, use the `kernel` predicate in [README.md](README.md). The current interrupt callback logs every successful zero-status completion; filter `OnItrComplete` while following startup to avoid hiding failures in that volume.
