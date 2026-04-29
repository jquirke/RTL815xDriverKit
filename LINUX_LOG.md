# Linux Realtek r8152 Log

Working tree: `/Users/jeremyquirke/github.com/realtek-r8152-linux`

This log tracks observations from the Linux Realtek USB Ethernet source. It is used as a comparison source for the Windows NDIS `rtu59` driver and the older macOS kext notes.

## RTL8157 vs RTL8159 ops split

Source: `r8152.c`, `rtl_ops_init(struct r8152 *tp)`.

The Linux driver treats `RTL_VER_16` and `RTL_VER_17` as adjacent members of the same late-generation family:

| Version      | Inferred chip | Main role in source |
| ------------ | ------------- | ------------------- |
| `RTL_VER_16` | RTL8157       | 5GbE path           |
| `RTL_VER_17` | RTL8159       | 10GbE path          |

Most host-driver behavior is shared between the two versions. Both cases install the same callbacks for enable/disable, link up/down, unload, EEE get/set, autoneg state, runtime suspend, MTU change, and PHY access.

| Operation             | `RTL_VER_16` / RTL8157   | `RTL_VER_17` / RTL8159   | Notes                           |
| --------------------- | ------------------------ | ------------------------ | ------------------------------- |
| `ops->init`           | `r8157_init`             | `r8159_init`             | Per-chip init path.             |
| `ops->enable`         | `rtl8157_enable`         | `rtl8157_enable`         | Shared.                         |
| `ops->disable`        | `rtl8153_disable`        | `rtl8153_disable`        | Shared older helper.            |
| `ops->up`             | `rtl8157_up`             | `rtl8157_up`             | Shared datapath bring-up.       |
| `ops->down`           | `rtl8157_down`           | `rtl8157_down`           | Shared datapath shutdown.       |
| `ops->unload`         | `rtl8157_unload`         | `rtl8157_unload`         | Shared unload.                  |
| `ops->eee_get`        | `r8156_get_eee`          | `r8156_get_eee`          | Shared EEE helper.              |
| `ops->eee_set`        | `r8156_set_eee`          | `r8156_set_eee`          | Shared EEE helper.              |
| `ops->in_nway`        | `rtl8153_in_nway`        | `rtl8153_in_nway`        | Shared older autoneg helper.    |
| `ops->hw_phy_cfg`     | `r8157_hw_phy_cfg`       | `r8159_hw_phy_cfg`       | Main per-chip PHY tuning hook.  |
| `ops->autosuspend_en` | `rtl8157_runtime_enable` | `rtl8157_runtime_enable` | Shared runtime power behavior.  |
| `ops->change_mtu`     | `rtl8157_change_mtu`     | `rtl8157_change_mtu`     | Shared.                         |
| `ops->phy_read`       | `r8157_phy_read`         | `r8157_phy_read`         | Shared PHY access.              |
| `ops->phy_write`      | `r8157_phy_write`        | `r8157_phy_write`        | Shared PHY access.              |
| descriptor init       | `r8157_desc_init(tp)`    | `r8157_desc_init(tp)`    | Shared RX/TX descriptor format. |

### Capability differences

`RTL_VER_17` mostly extends the `RTL_VER_16` shape upward to 10GbE:

| Field              | RTL8157 (`RTL_VER_16`)                 | RTL8159 (`RTL_VER_17`)                                      |
| ------------------ | -------------------------------------- | ----------------------------------------------------------- |
| `tp->rx_buf_sz`    | `32 * 1024`                            | `48 * 1024`                                                 |
| `support_2500full` | `1`                                    | `1`                                                         |
| `support_5000full` | `1`                                    | `1`                                                         |
| `support_10gfull`  | unset                                  | `1`                                                         |
| `eee_en`           | `true`                                 | `true`                                                      |
| `eee_adv`          | `MDIO_EEE_1000T` plus `MDIO_EEE_100TX` | `MDIO_EEE_100TX` plus `MDIO_EEE_1000T` plus `MDIO_EEE_10GT` |
| `eee_adv2`         | `MDIO_EEE_2_5GT` plus `MDIO_EEE_5GT`   | `MDIO_EEE_2_5GT` plus `MDIO_EEE_5GT`                        |

So the obvious host-visible changes are larger receive buffering and 10G capability/EEE advertisement. The data-path hooks remain 8157-family hooks.

### Shared descriptor format

Both versions call `r8157_desc_init(tp)`, which sets:

| Descriptor side    | Setting                     |
| ------------------ | --------------------------- |
| RX descriptor size | `sizeof(struct rx_desc_v2)` |
| RX alignment       | `16`                        |
| RX VLAN tag helper | `r8157_rx_vlan_tag`         |
| RX checksum helper | `r8157_rx_csum`             |
| RX length helper   | `r8157_rx_len`              |
| TX descriptor size | `sizeof(struct tx_desc_v2)` |
| TX alignment       | `16`                        |
| TX VLAN tag helper | `r8152_tx_vlan_tag`         |
| TX checksum helper | `r8157_tx_csum`             |
| TX length helper   | `r8157_tx_len`              |

This is useful when reading the Windows 10GbE NDIS driver: an RTL8159-specific `.sys` does not necessarily imply a new host packet descriptor model. Linux suggests RTL8159 remains on the RTL8157 descriptor/data-path model.

## Per-chip init differences

`r8157_init` and `r8159_init` are structurally very similar. Both do the standard late-chip bring-up sequence: unplug guard, debug state, USB/ECM mode gating, U1/U2 disable, autoload wait, PHY status checks, BMCR power-down clear, LAN_ON wait, USB power/queue/runtime suspend configuration, LPM/LTM disable, flow-control setup, RX aggregation setup, TX descriptor signature setup, MDIO force mode, tally reset, and `tp->coalesce = 15000`.

Observed differences:

| Area                          | RTL8157 `r8157_init`                                               | RTL8159 `r8159_init`                                                                                        | Notes                                                                                                     |
| ----------------------------- | ------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| Debug init marker             | `rtl_set_dbg_info_init(tp, 0x1)`                                   | `rtl_set_dbg_info_init(tp, 0x9)`                                                                            | Different internal phase marker.                                                                          |
| Flash/power-cut wait ordering | Calls `r8156b_wait_loading_flash(tp)` before MSC/U1U2 timer setup. | Calls `r8159_wait_backup_restore(tp)` before timer setup, then later calls `r8156b_wait_loading_flash(tp)`. | RTL8159 has an extra backup/restore state wait tied to `PCUT_STATUS` and `USB_GPHY_CTRL/BACKUP_RESTRORE`. |
| Timer setup                   | After PHY initial checks.                                          | Earlier, before flow-control setup and flash wait.                                                          | Order differs, not obviously behavioral at host level.                                                    |
| ALDPS bypass clear            | Explicitly clears PLA `0xd3c8` bit 0 in init.                      | Does not clear it in init, but `r8159_hw_phy_cfg` clears it.                                                | Same knob appears, moved to PHY config path.                                                              |
| USB `0xcd06` bit 11           | Not present.                                                       | Clears USB `0xcd06` bit 11.                                                                                 | 8159-specific USB-side bit.                                                                               |
| RX aggregation bits           | Clears `RX_AGG_DISABLE` plus `BIT(10)`.                            | Clears `RX_AGG_DISABLE` plus `BIT(10)` plus `BIT(11)`.                                                      | 8159 clears one extra RX aggregation control bit.                                                         |

The `r8159_wait_backup_restore` helper checks USB `USB_MISC_0` for `PCUT_STATUS`; if set, it polls USB `USB_GPHY_CTRL` until `BACKUP_RESTRORE` appears or times out after 100 iterations. This is one of the clearer non-8157 lifecycle additions.

## Per-chip PHY config differences

Both `r8157_hw_phy_cfg` and `r8159_hw_phy_cfg` follow the same broad outline:

1. Wait for flash/autoload state.
2. Invoke a per-version patch hook.
3. Clear `PCUT_STATUS`.
4. Check PHY status.
5. Load firmware/ram-code path depending on PHY state.
6. Clear BMCR power-down.
7. Disable ALDPS before changing PHY parameters.
8. Wait for `PHY_STAT_LAN_ON`.
9. Clear PFM/PWM and set advanced power-saving parameters.
10. Apply large version-specific SRAM/OCP tuning block.
11. Re-enable green/EEE/ALDPS/flow-control style settings and mark `PHY_RESET`.

Important differences:

| Area                           | RTL8157 `r8157_hw_phy_cfg`                                                     | RTL8159 `r8159_hw_phy_cfg`                                                                                                                                    |
| ------------------------------ | ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Patch hook                     | `r8157_patch_code(tp)`                                                         | `r8159_patch_code(tp)`                                                                                                                                        |
| Patch hook body                | Real firmware/patch body was factored into `r8152_firmware.c`.                 | Stub in upstream source: empty function.                                                                                                                      |
| State-dependent firmware call  | `r8157_firmware(tp, true/false)`                                               | `rtl_ram_code_speed_up(tp, false/true)`                                                                                                                       |
| EEE disable before PHY updates | Explicit `rtl_eee_enable(tp, false)` before LAN_ON wait.                       | Not present in the corresponding early section.                                                                                                               |
| ALDPS bypass bit               | Not in same location.                                                          | Clears PLA `0xd3c8` bit 0 after disabling ALDPS force mode.                                                                                                   |
| Version-specific tuning        | 5G/2.5G RFI, green mode, AQC/Bcom recognition, XG compatibility, SRAM2 tuning. | 10G/5G/2.5G power levels, AFE power saving, RFI/PGA tuning, XG INRX, XG test mode, cable test, thermal sensor, 100M speed-change shaping, lsbmask parameters. |

The Linux 8159 path therefore looks less like a brand-new host driver architecture and more like a 10GbE PHY/analog tuning extension on top of the RTL8157-family USB datapath.

## RTL8157/RTL8159 paired-case pattern

A source-wide scan shows that `RTL_VER_16` and `RTL_VER_17` are usually paired together for ordinary host-side behavior. Examples include reset/disable handling, RX early timeout, RX early size scaling, VLAN filtering, BMU reset, breakpoint disable, EEE helper dispatch, flow-control pause parameter handling, MTU limits, and netdev `max_mtu` setup.

The useful reading is that Linux treats RTL8157 and RTL8159 as the same late-family host datapath in most places. The unpaired cases are higher-signal, but not all of them are architecturally important.

| Area               | Shape                                                                                                                                          | Interpretation                                                                                                                                                                                                                           |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Version detection  | OCP version `0x1030` maps to `RTL_VER_16`; `0x2020` maps to `RTL_VER_17`; nearby IDs are commented out.                                        | Identification only, not a behavioral split. The comments suggest earlier/internal revision IDs.                                                                                                                                         |
| Ops table          | Separate `rtl_ops_init` cases, but almost all callbacks are duplicated.                                                                        | Mostly a capability/init split: 8159 swaps in `r8159_init`/`r8159_hw_phy_cfg`, increases RX buffer to 48 KiB, and enables 10G support.                                                                                                   |
| EEE Plus           | `rtl_set_eee_plus` disables EEE Plus for `RTL_VER_17`; other versions enable it only for 10 Mbps links.                                        | Minor power-management quirk, not a substantial datapath deviation. It may reflect 10M-specific behavior that is irrelevant or undesirable on RTL8159. Linux still exposes normal 10/100/1000 plus 2.5/5/10G capability paths elsewhere. |
| PHY tuning         | `r8157_hw_phy_cfg` and `r8159_hw_phy_cfg` are separate.                                                                                        | Expected per-chip PHY/analog bring-up. This is the main meaningful divergence outside pure capability flags.                                                                                                                             |
| RAM/patch payloads | `rtl_ram_code_speed_up` selects `ram16`/`ram17`; `r8157_patch_code` has a real RTL8157 USB patch payload; `r8159_patch_code` is an empty stub. | Firmware/patch selection remains per-chip, but this does not imply a new host packet model.                                                                                                                                              |


## Vendor Realtek r8152 vs upstream Linux r8152

Comparison setup:

| Input                            | Path                                                                                                                                                           |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Upstream kernel source           | `/tmp/rtl8152_compare/torvalds_master_r8152.c` fetched from `https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/usb/r8152.c`                  |
| Vendor source before local split | `/tmp/rtl8152_compare/realtek_wget_HEAD_unsplit_r8152.c`, reconstructed with `git show HEAD:r8152.c` from `/Users/jeremyquirke/github.com/realtek-r8152-linux` |
| Full unified diff                | `/tmp/rtl8152_compare/torvalds_vs_realtek_wget_HEAD_unsplit.diff`                                                                                              |

The local split into `r8152_firmware.c` was not reverted or used as the diff input. The vendor-side input is the original unsplit file from git history.

Line counts:

| Source                                | Lines  |
| ------------------------------------- | ------ |
| Upstream `torvalds/linux` `r8152.c`   | 10,224 |
| Vendor Realtek/wget unsplit `r8152.c` | 32,707 |
| Unified diff                          | 34,632 |

High-level differences:

| Area                   | Upstream kernel                                                                                                                                                                                           | Vendor Realtek/wget tree                                                                                                                                                                                    |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Highest chip version   | `RTL_VER_16`                                                                                                                                                                                              | `RTL_VER_17`                                                                                                                                                                                                |
| RTL8159 / 10G support  | Not present                                                                                                                                                                                               | Present as `RTL_VER_17` with `support_10gfull`                                                                                                                                                              |
| Firmware model         | External firmware loader using `request_firmware`, `rtl_nic/*.fw`, and `MODULE_FIRMWARE` declarations.                                                                                                    | Large inline firmware/patch arrays and helpers inside `r8152.c` before our split.                                                                                                                           |
| RTL8157 ops            | Present, but upstream `RTL_VER_16` still uses several `r8156`/older helpers: `r8156_init`, `rtl8156_enable`, `rtl8156_up/down`, `rtl8153_unload`, `r8153_get_eee`, `r8152_set_eee`, `rtl8156_change_mtu`. | Vendor gives RTL8157 a more explicit late-family path: `r8157_init`, `rtl8157_enable`, `rtl8157_up/down`, `rtl8157_unload`, `r8156_get_eee`, `r8156_set_eee`, `rtl8157_change_mtu`, `r8157_phy_read/write`. |
| RTL8159 ops            | Absent.                                                                                                                                                                                                   | Adds `r8159_init`, `r8159_wait_backup_restore`, `r8159_hw_phy_cfg`, `r8159_patch_code` stub, and shared RTL8157-family datapath callbacks.                                                                  |
| 10G ethtool/speed path | No `support_10gfull`; no `SPEED_10000` handling in this driver.                                                                                                                                           | Adds 10G advertised/supported link mode handling, `MDIO_AN_10GBT_CTRL_ADV10G`, `MDIO_AN_10GBT_STAT_LP10G`, and `SPEED_10000`.                                                                               |

Vendor-only RTL8159-related additions seen in the pre-split source:

| Symbol / field              | Role                                                                                                   |
| --------------------------- | ------------------------------------------------------------------------------------------------------ |
| `RTL_VER_17`                | New enum value after `RTL_VER_16`.                                                                     |
| OCP version `0x2020`        | Maps to `RTL_VER_17`.                                                                                  |
| `support_10gfull`           | Per-device capability bit enabling 10G paths.                                                          |
| `r8159_patch_code`          | Stub in this source.                                                                                   |
| `r8159_hw_phy_cfg`          | 10G-specific PHY/analog/SRAM/OCP tuning path.                                                          |
| `r8159_wait_backup_restore` | Polls backup/restore state when `PCUT_STATUS` is set.                                                  |
| `r8159_init`                | 8159 init path, structurally close to `r8157_init` with extra backup/restore and USB-side differences. |

Important interpretation:

Upstream kernel `master` currently knows about the RTL8157-generation descriptor format and `RTL_VER_16`, but its `RTL_VER_16` support is less factored into named `r8157_*` lifecycle helpers than the vendor source. The vendor source appears to advance the late-family branch: it makes RTL8157 more explicit, then adds RTL8159 as a mostly paired `RTL_VER_17` case with 10G capability and chip-specific init/PHY tuning.


## Git history: first RTL_VER_17 and RTL8159 appearances

History checks were run against `/Users/jeremyquirke/github.com/realtek-r8152-linux` using `git log --reverse -S/-G` over `r8152.c`.

| Query                      | First commit                               | Date       | Import                  | Interpretation                                                                                                                                                    |
| -------------------------- | ------------------------------------------ | ---------- | ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `RTL_VER_17`               | `a3dd2c0d8cc8702cff4a7a4a6576e6e86076091a` | 2024-07-01 | `Import version 2.18.1` | First enum/use of `RTL_VER_17`, but not current RTL8159 semantics. In this import, OCP `0x1030` maps to `RTL_VER_17`; later sources map `0x1030` to `RTL_VER_16`. |
| `r8159` symbols            | `8ee2d3108fdb55c71ba173f4015534054e9db6bf` | 2025-05-25 | `Import version 2.20.1` | First real `r8159_*` functions: `r8159_patch_code`, `r8159_hw_phy_cfg`, `r8159_wait_backup_restore`, `r8159_init`.                                                |
| `support_10gfull`          | `8ee2d3108fdb55c71ba173f4015534054e9db6bf` | 2025-05-25 | `Import version 2.20.1` | First clear 10G capability plumbing.                                                                                                                              |
| OCP `0x2020 -> RTL_VER_17` | `8ee2d3108fdb55c71ba173f4015534054e9db6bf` | 2025-05-25 | `Import version 2.20.1` | First current-shape RTL8159 version mapping.                                                                                                                      |
| Explicit `RTL8159` string  | none found                                 | -          | -                       | No literal `RTL8159` text found in repo history.                                                                                                                  |

Short version: `RTL_VER_17` as a name appears in vendor import 2.18.1, but it initially represents the `0x1030` path that later becomes `RTL_VER_16`/RTL8157. The first commit that looks like actual RTL8159/10GbE support is `8ee2d3108fdb55c71ba173f4015534054e9db6bf` (`Import version 2.20.1`, 2025-05-25).
