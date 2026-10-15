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

## Deactivation, transient enumeration failure, and NCM fallback

The loader's **Uninstall** button submitted a system extension deactivation request. The sequence was not immediate or clean:

- `systemextensionsctl list` showed only the unrelated AQC111 extension; `en14` and RTL8159 dext processes disappeared.
- The still-visible USB device initially reported current configuration 1 and preferred configuration 2. NCM had not attached at that point.
- Early unplug/replug attempts logged port power-off/power-on transitions without a completed `0x0bda/815a` enumeration. The device was absent from IORegistry and `lsusb`.
- Reactivating the RTL8159 extension and reenumerating restored configuration 1, `RTL8159NIC`, and `en14`; two RTL8159 dext processes were observed.
- After the loader deactivated RTL8159 again, the stuck system extension entries and all RTL8159 dext processes were gone. A later reenumeration completed at 10 Gbps, and IORegistry reported `kUSBCurrentConfiguration = 2` and `kUSBPreferredConfiguration = 2`.
- IORegistry placed `en14` under `AppleUSBNCMData` and `AppleUSBNCMControl`, both from `com.apple.driver.usb.cdc.ncm`. `ifconfig en14` showed the hardware MAC `1c:86:0b:3b:e7:f6`, `media: autoselect (none)`, and `status: inactive`. No RTL8159 dext process was running, and only AQC111 remained in `systemextensionsctl list`.

This confirms that Apple's NCM driver can claim the adapter after RTL8159 deactivation. It does not explain why the adapter was temporarily invisible on the earlier attempts, or whether the prior corpse buildup came from a teardown defect or dirty system state. No reboot was needed for the eventual NCM attachment; the planned reboot was canceled once the stuck entries and corpses cleared.

### Controlled load/unload and NCM return

With the device continuously plugged in, installing RTL8159 switched the same USB device instance from configuration 2/NCM to configuration 1/RTL8159NIC. Uninstalling RTL8159 removed both dext processes and `en14`, but the device stayed enumerated in configuration 1 (preferred configuration 2), with no NCM interface. Waiting roughly three minutes did not change that state. A physical replug created a new USB device instance in configuration 2, and `en14` returned under `AppleUSBNCMControl` and `AppleUSBNCMData` with hardware MAC `1c:86:0b:3b:e7:f6`.

Two further load/unload/replug cycles gave the same result. NCM was present after the first replug in each cycle, at 07:57:23 and 07:58:21, respectively 21 and 14 seconds after the unloaded-state checks. This rules out a wait of several minutes as a requirement in those runs; the earlier failed enumerations remain unexplained.

### Explicit configuration-2 handoff

`RTL8159Driver::Stop` now calls `SetConfiguration(2, true)` while its device session is open, then closes the session. A `CODE_SIGNING_ALLOWED=NO` Xcode build passed, and the signed device personality with build timestamp `[Oct 16 2026 08:09:14]` was installed through the loader.

With the device continuously plugged in, RTL8159 first claimed the existing USB device instance in configuration 1. On loader uninstall at 08:10:24, the logs showed `SetConfiguration(2) -> 0x0` and `Close -> 0x0`. The `RTL8159NIC::Stop` entry appeared about 5 ms later. IORegistry then showed **the same USB device instance** (`0x1000291cb`) in configuration 2, with `AppleUSBNCMControl` and `AppleUSBNCMData` owning `en14`. `ifconfig en14` showed the hardware MAC `1c:86:0b:3b:e7:f6`. RTL8159 was absent from `systemextensionsctl list`, and no RTL8159 dext processes remained. No physical replug was needed.

The USB configuration handoff succeeded, but **the dext shutdown was not clean**. DriverKit logged `RTL8159NIC:force close`, `Corpse allowed 5 of 5`, then `Corpse failure, too many 6`; `sysextd` reported `kernelmanagerd.KMError Code=38` before finishing uninstall. A crash report for device-personality PID 39153 recorded `EXC_BAD_ACCESS` at address `0x10` on `RTL8159Driver-Default`, inside the asynchronous `IOService::Stop_Impl` cleanup block. Earlier RTL8159 crash reports show the same stack and fault address, so this failure predates the configuration-2 change. Both dext processes nevertheless exited.

NCM attached, but its `sendMER` request encountered three endpoint-0 `0xe0005000` pipe-stall errors at 08:10:24.295. No further USB/NCM errors appeared in the inspected window through 08:10:40. The interface remained present; link and traffic were not tested. Repeat the handoff after fixing the existing Stop/corpse issue, and test physical disconnect separately.

### Borrowed-provider ownership experiment

The device personality stored its `Start` provider from `OSDynamicCast` and called `OSSafeReleaseNULL` on it during `Stop`; the NIC did the same to its interface provider. Those casts do not add a retain, while DriverKit retains each provider through `Stop`. Both releases have been removed; each personality still closes its own provider session, and the NIC still releases its separately owned `CopyDevice()` reference. The unsigned Xcode build passed.

The signed build with device-personality timestamp `[Oct 16 2026 08:14:21]` loaded in configuration 1 and was uninstalled at 08:14:50 without unplugging. `SetConfiguration(2)` and `Close` both returned `0x0`; the same USB device instance moved to configuration 2, NCM owned `en14`, and both RTL8159 processes exited. Through 08:15:05 there were **no new RTL8159 crash reports or corpse messages**. The newest crash report remained the 08:10:24 report from the previous build. This one run supports the borrowed-provider release as the cause of the `Stop_Impl` crash, but more cycles are needed to establish repeatability.

The kernel still logged `RTL8159NIC:force close`, and `sysextd` logged `kernelmanagerd.KMError Code=38` before completing uninstall. NCM's `sendMER` request again returned `0xe0005000`; NCM nonetheless created `en14`. These messages remain separate from the resolved crash symptom.

The same signed build was loaded and unloaded again at 08:16:23. `SetConfiguration(2)` and `Close` returned `0x0`, the same USB device instance returned to configuration 2, NCM reclaimed `en14`, and both RTL8159 processes exited. No corpse messages or new crash report appeared. The NIC force-close message, `sysextd` Code 38 warning, and NCM `sendMER` stall recurred. The provider-release fix has therefore passed **two consecutive unload cycles** for the crash/corpse symptom; the remaining messages need separate interpretation.

## Next controlled checks

1. Restore hardware MAC reading only after isolating the first vendor request. Compare a no-request build with a build that waits after `SetConfiguration(1)` before the read, and log request start/completion before interpreting a reset.
2. Repeat activation, deactivation, and reenumeration from a clean state. Record USB port events, extension state, dext process count, configuration value, and the IORegistry parent of the resulting `en` interface to isolate the transient enumeration/teardown behavior.
3. Implement and validate hardware initialization, real MAC reading, link state, RX aggregate parsing, and TX framing incrementally. Require on-wire evidence before marking Ethernet traffic as working.

For live dext logs, use the `kernel` predicate in [README.md](README.md). The current interrupt callback logs every successful zero-status completion; filter `OnItrComplete` while following startup to avoid hiding failures in that volume.
