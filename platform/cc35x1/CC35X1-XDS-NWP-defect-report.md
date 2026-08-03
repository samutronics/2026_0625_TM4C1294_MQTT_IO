# Defect Report — LP-EM-CC35X1 debug/flash + Wi-Fi bring-up

**Audience:** Code Composer Studio (CCS) debug-server team · XDS110 debug-probe firmware team · CC35xx SimpleLink Wi-Fi NWP / host-driver team
**Reporter context:** Bringing up a custom FreeRTOS + lwIP (ethernet-bridge) STA application on a bare LP-EM-CC35X1, driven from CCS via the on-board XDS110. Five distinct, reproducible defects were isolated during bring-up. Each is attributed to the most likely owning component below; several are cross-team.

---

## 1. Environment (exact)

| Item | Value |
|---|---|
| Board | LP-EM-CC35X1 (device `CC3551E`, Cortex-M33 @160 MHz + Wi-Fi NWP co-processor) |
| SDK | SimpleLink Wi-Fi SDK **10.10.01.08** (`C:/ti/simplelink_wifi_sdk_10_10_01_08`) |
| IDE | CCS **2100** (Theia-based) (`C:/ti/ccs2100`) |
| Compiler | TI Arm Clang **5.1.1.LTS** (`tiarmclang`) |
| Debug probe | On-board **XDS110**: `VID 0x0451 PID 0xbef3`, "XDS110 Embed with CMSIS-DAP", **firmware 3.0.0.43**, Runtime mode |
| Host OS | Windows 11 Pro 10.0.26200 |
| RTOS / stack | FreeRTOS (ARM_CM33_NTZ port) + lwIP 2.1.3, `NO_SYS=0`, `LWIP_TCPIP_CORE_LOCKING=1` |
| App topology | STA via `Wlan_*` host-driver API; NWP acts as L2 Ethernet bridge (`Wlan_EtherPacketSend/Recv`), lwIP does L3+ on the M33 |
| AP under test | Phone hotspot, WPA2-Personal **with PMF (802.11w)** enabled, SSID/pass redacted |

---

## 2. Defect A — XDS110 `Error -260` is unrecoverable except by physical USB re-enumeration
**Likely owners: XDS110 firmware + CCS debug server**

### Observed
After a prior debug session left the target in a fault (see Defect B/C), the next `Launch/Load` in CCS:
- **hangs for 10+ minutes** with no debug session created and **no GEL output stream** ever opened, then fails with:
  ```
  Texas Instruments XDS110 USB Debug Probe/CS_DAP_0
  Error connecting to the target: (Error -260 @ 0x0)
  An attempt to connect to the XDS110 failed. The cause may be one or more of:
  no XDS110 is connected, invalid firmware update, invalid XDS110 serial number,
  or faulty USB cable...
  ```
- The error text points at cable/firmware/serial-number causes, **all of which were false.** `xdsdfu -e` at that exact moment reported the probe perfectly healthy and enumerated:
  ```
  <<<< Device 0 >>>>
  VID: 0x0451    PID: 0xbef3
  Device Name:   XDS110 Embed with CMSIS-DAP
  Version:       3.0.0.43
  Manufacturer:  Texas Instruments
  Serial Num:    E10000H6
  Mode:          Runtime
  Found 1 device.
  ```
- **`xds110reset.exe` returned success (exit 0) but did NOT clear the condition** — the very next `Load` produced -260 again.
- **Only a physical USB unplug/replug** (re-enumeration of the composite device) cleared it. After re-plug, connect succeeded normally.

### Impact
- The failure mode is a **10+ minute silent hang** before any error, with **no progress indication** and no partial session/telemetry — indistinguishable from a slow-but-working connect.
- The error message misdirects the user to cable/firmware, when the real cause is stale internal state in the probe/debug-server connection, recoverable only by re-enumeration.

### Requested fixes
- **XDS110 fw / CCS:** make `xds110reset` (or an equivalent soft path) actually reset the debug-link state that -260 gets stuck on, so a physical replug isn't required.
- **CCS debug server:** fail fast (bounded timeout, e.g. ≤60 s) instead of hanging 10+ minutes; surface a progress/heartbeat; and when the probe is enumerable but the target link is wedged, say so instead of listing cable/firmware causes.

---

## 3. Defect B — A core (M33) reload does **not** reset the Wi-Fi NWP → stale supplicant state → WPA2 4-way-handshake timeout
**Likely owners: CCS debug/GEL (no NWP reset on load) + NWP firmware (retains stale state / no recovery)**

### Observed — this is the central defect
- On a **clean power-cycled boot**, the STA connected fully: 4-way handshake completed, DHCP lease obtained (`192.168.1.140`), application TCP server reachable. Verified via debugger (`netif_default->flags == 0x2F` = UP|LINK_UP, valid `ip_addr`).
- On **every subsequent `Load`/`Debug` (M33 reload) without a USB power-cycle**, association fails **identically and repeatably**:
  - `WLAN_EVENT_CONNECT` never fires (host counter `connects = 0`).
  - Exactly one `WLAN_EVENT_DISCONNECT` fires with **`ReasonCode = 15` (802.11 "4-Way Handshake timeout"), `IsStaIsDiscnctInitiator = 0`** (AP-initiated).
  - `netif_default->flags == 0x2A` (link down), `ip_addr == 0`.
- The PSK and security type are correct — proven by the one clean-boot success with the identical binary. The **only variable is NWP internal state carried across the M33 reload.**

### Root cause (analysis)
The CC35xx `Load` **does reprogram the NWP firmware images** — the programmer log shows it writing `primary_ti_wireless_fw_image`, `primary_vendor_image`, boot sector, and tables (~500 KB across images) on every load. **Yet the stale-state failure persists**, which pinpoints the real defect: **the load rewrites NWP flash but does not cleanly restart the NWP *runtime*** (RAM / supplicant / PMF-SA / connection-manager state carries over from the previous run). Only a **hardware power-cycle** restarts the NWP from the freshly-written images. With stale runtime state, the freshly-reloaded host driver's new connection makes the 4-way handshake time out.

**Confirmation:** with the corrected firmware in flash + a USB power-cycle + **no** subsequent reload, the STA associated, held the link, obtained DHCP, and served HTTP (200) — reproducibly. The failure only appears on a reload-without-power-cycle.

### Impact
- Makes iterative firmware development on Wi-Fi code **extremely painful**: every reflash requires a manual USB power-cycle to get a testable radio, otherwise all connections fail with a misleading reason-15.
- The failure is silent to anyone not decoding 802.11 reason codes.

### Requested fixes
- **CCS / GEL / debug config:** after writing the NWP images on `Load`, **restart the NWP from the new images** (drive the NWP reset line / issue the NWP reset command) as part of the target-reset sequence for CC35xx, so a reload gives a clean radio — matching user expectation that "load = fresh device." Writing the fw flash without restarting the NWP runtime is the specific gap.
- **NWP firmware:** detect host-transport re-initialization (host driver re-`Wlan_Start`) and **self-reset supplicant/CME state**, so a host restart doesn't inherit a half-finished handshake. Alternatively expose a documented, reliable "full NWP reset" host call that works from a wedged state (see Defect C).

---

## 4. Defect C — `Wlan_Stop()` on a wedged NWP **asserts** in the host-driver transport layer instead of returning an error
**Likely owner: host driver + NWP firmware**

### Observed
As a software workaround for Defect B, the app called `Wlan_Stop(1)` before `Wlan_Start()` at boot to force-clean the NWP. On an already-stale NWP this **faulted the host driver** — the M33 halted in an assert:
```
#0 ASSERT_GENERAL()        osi_dpl.c:110
#1 FwEvent_StateMachine()  source/ti/drivers/net/wifi/wifi_host_driver/trnspt_layer/fw_event_if.c:525
#2 trnspt_Task()           source/ti/drivers/net/wifi/wifi_host_driver/trnspt_layer/trnspt_thread.c:285
```
After this the init path never reached a connect attempt (`connects = 0`, `disconnects = 0`, radio down).

### Impact
- A **recovery API (`Wlan_Stop`) that asserts** when used to recover from a bad state is unusable for its purpose; it converts a recoverable stale-state into a hard host-side fault.

### Requested fixes
- **Host driver:** `Wlan_Stop()` (and the transport `FwEvent_StateMachine`) must **return an error code** on unexpected NWP/transport state rather than `ASSERT_GENERAL`. Assertions on external-coprocessor state are not appropriate in a shipping driver path.
- Document the supported sequence to fully reset the NWP from software, and guarantee it works from any state (including mid-handshake / wedged).

---

## 5. Defect D — XDS110 CDC backchannel UART goes silent after target reset until USB re-enumeration
**Likely owners: XDS110 CDC-UART firmware + CCS serial**

### Observed
- The XDS110 "Class Application/User UART" (COM14) delivers boot log correctly on the **first** run after enumeration.
- After **any target reset** (`Load`, `Restart`, `reset`), the CDC UART **stops delivering new bytes**; the OS port handle stays "open" but no data flows. Re-opening the port re-serves only the **previously buffered** text, so a reader sees the *old* boot's log and can mistake it for the current one.
- Recovery is the same USB re-enumeration as Defect A.

### Impact
- Loses serial visibility exactly when it's most needed (debugging boot/bring-up across reflashes), forcing reliance on the debugger to read RAM.

### Requested fix
- **XDS110 CDC / CCS:** re-establish/flush the CDC-UART bridge across a target reset so the backchannel keeps streaming without a physical replug; clearly delineate buffered-vs-live data.

---

## 6. Defect E — CME connection-ownership collision: `RoleUp` auto-connect competes with an explicit `Wlan_Connect`
**Likely owner: NWP firmware (CME / connection manager)**

### Observed
With a **stored profile + auto/fast connection policy** left in NVS (e.g. after running the SDK provisioning demos), a plain `Wlan_Start → Wlan_RoleUp(STA) → Wlan_Connect(...)` sequence produced, on the console:
```
CME :Error first ! CmeStationFlowSmValidateTransitionUserEvent: Valid state:0 ,
     New User owner request (current user ENUM(Cme_Users_e, 1) new user ENUM(Cme_U...
CME :CCmeStationFlowSM: ERROR! UnExpected event
     currentState=ENUM(Cme_STA_states_e, 2)   // CME_STA_SUPPLICANT_MANAGED_STATE
     Event=ENUM(Cme_STA_events_e, 8)           // CME_STA_WLAN_PEER_DISCONNECT_REQ
     User: ENUM(Cme_Users_e, 1)                // CME_STA_WLAN_CONNECT_USER
```
i.e. the explicit connect (`CME_STA_WLAN_CONNECT_USER`) and the policy-driven auto/fast connect (`CME_STA_FAST_CONNECT_USER`) both try to own the STA flow, the CME rejects the transition, and the link is torn down. The app had to **explicitly** `Wlan_Set(WLAN_SET_CONNECTION_POLICY, {0,0,0,0})` + `Wlan_ProfileDel(0xFF)` before `RoleUp` to avoid it.

### Impact / requested fix
- **NWP firmware:** an explicit `Wlan_Connect` should cleanly **pre-empt** (or be safely serialized against) an in-flight policy-driven auto/fast connect, instead of erroring and deauthing. At minimum, document that explicit-connect applications must disable the connection policy and clear profiles first.

---

## 7. Secondary observation — debugger halt deauthenticates the STA
**Owner: NWP firmware (informational)**

Halting the M33 (`pause`) for more than a few seconds causes the AP to deauth the station (host stops servicing the NWP transport → keep-alive/handshake maintenance stalls). This makes live network debugging fragile. A "debug-hold" mode where the NWP maintains the association independently of the halted host would help, though we recognize this is inherent to the split-processor design.

---

## 8. Consolidated reproduction (Defects A–D)

1. Power-cycle board; `Load` + run the STA app → connects, gets DHCP, reachable. (baseline OK)
2. In CCS, `Load` again (M33 reload, **no** USB power-cycle).
3. Observe: `WLAN_EVENT_DISCONNECT ReasonCode=15`, no `WLAN_EVENT_CONNECT`, `netif` link-down — **Defect B**.
4. Backchannel COM UART now silent; only old buffered log returned — **Defect D**.
5. Attempt `Wlan_Stop(1)` recovery → host-driver `ASSERT_GENERAL` in `trnspt_Task` — **Defect C**.
6. After a prior faulted session, next `Load` hangs 10+ min → `Error -260`; `xds110reset` exit 0 but ineffective; physical USB replug required — **Defect A**.

## 9. Priority summary

| # | Defect | Owner(s) | Severity |
|---|---|---|---|
| B | Core reload doesn't reset NWP → reason-15 handshake timeout | CCS/GEL + NWP fw | **High** (blocks Wi-Fi dev iteration) |
| A | XDS110 -260 unrecoverable w/o USB replug; 10-min silent hang; misleading msg | XDS110 fw + CCS | **High** |
| C | `Wlan_Stop()` asserts instead of erroring on wedged NWP | host driver | Medium-High |
| D | CDC backchannel UART dead after reset until re-enumeration | XDS110 fw + CCS | Medium |
| E | CME ownership collision (policy auto-connect vs explicit connect) | NWP fw | Medium |
| — | Debugger halt deauths STA | NWP fw | Low (design-inherent) |

*Evidence (reason codes, CME strings, assert stack, `xdsdfu -e` output, `netif` flag/ip values) captured live via CCS debugger `evaluate` and `xdsdfu`/`xds110reset` CLI on the dates above.*
