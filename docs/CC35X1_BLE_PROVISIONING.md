# CC35x1 — BLE Wi-Fi Provisioning: Design & Integration Note

**Status: design note (board-independent work).** Companion to `CC35X1_PORTING_PROPOSAL.md`.
Decision (revised 2026-07-25): **BLE provisioning is the CC35x1 onboarding path, done first.**

Reference: SDK demo `ble_wifi_provisioning` (`simplelink_wifi_sdk_10_10_01_08`,
`examples/rtos/LP_EM_CC35X1/demos/ble_wifi_provisioning/`). **Toolchain validated:** the demo's
TICLANG projectspec imports and **builds clean** headlessly (only `-Ofast` deprecation warnings).

---

## 1. Goal

Let a user commission the field-I/O gateway onto their Wi-Fi with **no cable and no known IP**:
open a phone app, connect to the board over BLE, enter SSID + password, done. After that the board
behaves exactly like the TM4C version (web UI + MQTT/HA), just over Wi-Fi. BLE is CC35x1-only and
lives behind an interface; the TM4C never compiles it.

## 2. How the SDK demo works (what we're adopting)

**Flow (power-on → joined AP):**
1. Board boots, brings up Wi-Fi STA then BLE, and advertises as **`cc35xxble`**.
2. Phone (TI **SimpleLink Connect** app) connects, opens the provisioning GATT service, writes
   SSID / security-type / password, then **reads** the connection characteristic to trigger connect.
3. Device calls `Wlan_Connect()`; on `WLAN_EVENT_CONNECT` it brings the lwIP STA netif up and
   **notifies** the phone (status → connected). The WLAN subsystem persists the profile itself.

**GATT service (all 16-bit UUIDs, `nimble_host_provisioning.c`):**

| Item | UUID | Props | Payload |
|------|------|-------|---------|
| Service "Wifi Provisioning over BLE" | `0xCC00` | primary | — |
| SSID | `0xCC01` | Write | raw bytes (≤ SSID max) |
| Password | `0xCC02` | Write | raw bytes (WPA 8–63) |
| Connection/status | `0xCC03` | Read + Notify | **read triggers connect**; notify pushes 1-byte status: 0 idle / 1 start / 2 success / 3 failure |
| Security type | `0xCC04` | Write | 1 byte WLAN security enum (OPEN / WPA_WPA2 / WPA3) |

No pairing/bonding required. GAP name + DIS come from SysConfig.

**Credential storage:** the app's received SSID/pw live in RAM only and are wiped after connect
(`provClearCredentials`). The **WLAN profile is persisted automatically** by the Wi-Fi stack via
`NVINTF`/`NVOCMP` → **external OSPI flash** (`XMEMWFF3`), system-ID `NVINTF_SYSID_WIFI`, item
`NVID_WLAN_PROFILES_OFFSET_IN_FLASH (0x5)`. So a provisioned board **auto-reconnects on next boot**.

**Init order (as implemented):** `Board_init` → `network_stack_init` (lwIP) → `wlanStart`
(`Wlan_Start(handler)`) → `wlanRoleUpSta` (`network_stack_add_if_sta` → `Wlan_RoleUp(STA)`) →
`bleStart` (`BleIf_OpenTransport` → `nimble_host_start`) → `advConfigure`/`advEnable` → block on a
"start connect" sync object. Two FreeRTOS tasks matter: `mainThread` (init + provisioning loop) and
`nimble_host` (BLE host event queue, prio 8). Paced by three `os_sleep(1s)` + a 1 s host-sync wait.

**Phone app:** TI **SimpleLink Connect** (generic BLE app) — no custom app needed for bring-up.

## 3. What we reuse vs. strip

**Keep (core):** `ble_wifi_provisioning.c/.h`, `nimble_host_provisioning.c/.h`, `ble_cmd.c/.h`,
`adaptation/osi_filesystem.c` (+ `nvocmp_cc35xx.c`, `nvocmp.h`, `nvintf.h`), `network_lwip.c/.h`,
the WLAN start/roleup/connect subset of `wlan_cmd.c`, `freertos/main_freertos.c`, the `.syscfg`,
linker files, and the `adaptation/` porting/util layer.

**Strip (demo scaffolding):** `lwip_iperf_*` (5), `lwip_ping.*`, `socket_examples.*` (57 KB),
`date_time_service.*`, `network_mbedtls.c` + certs, `dhcpserver.*` (AP-only), and — the big win —
`cmd_parser.c` (190 KB) plus most of `wlan_cmd.c`: the demo funnels connect through **CLI string
formatting** (`-s "<ssid>" -t WPA2 -p <pw>`). We call **`Wlan_Connect()` directly** with a
`WlanSecParams_t` and drop the CLI entirely.

## 4. Changes required for our gateway (gaps in the demo)

1. **Provisioned-state gate (must-add).** The demo re-advertises BLE **every boot**. We want: on
   boot, if a WLAN profile / our own "provisioned" NV flag exists → **skip BLE, go straight to STA
   connect**; only advertise for provisioning when unprovisioned **or** on an explicit
   **factory-reset / button-hold** trigger. (Mirrors the TM4C factory-reset GPIO concept.)
2. **Re-provision trigger.** Long-press a user button (SW1 `GPIO2` / SW2 `GPIO36`) → clear WLAN
   profile + flag → re-enter BLE advertising. This is the field "change my Wi-Fi" path.
3. **Status feedback without a terminal.** Drive the on-board **RGB LED (D4:** G `GPIO30`, R
   `GPIO34`, B `GPIO35`) for provisioning/connecting/connected/failed — the no-UART substitute
   (same spirit as the TM4C's approach).
4. **Direct connect API.** Replace `wlanConnect()` CLI-string path with a direct `Wlan_Connect()`
   call; delete `cmd_parser`.
5. **Hand-off to the app.** On `WLAN_CONNECT_SUCCESS`, start the rest of the stack: DHCP (already
   via `network_set_up`), then **NetBIOS + httpd + MQTT** (the shared `common/` services) — i.e.
   provisioning completion is the CC35x1 equivalent of the TM4C "Ethernet link up" trigger feeding
   `net_link`.
6. **Naming.** Advertise a recognizable BLE name derived from the **client ID** (like the NetBIOS
   name today) instead of the fixed `cc35xxble`, so multiple boards are distinguishable during
   commissioning.

## 5. Where it sits in the target architecture

Per the monorepo plan (`CC35X1_PORTING_PROPOSAL.md` §5–7), BLE provisioning is **platform-specific**
and stays under `platform/cc35x1/`, exposed to shared code through two seams:

- **`net_link`** — provisioning + association is the CC35x1 implementation of the same
  "am I connected / on-connect bring up transport" interface the TM4C implements with Ethernet
  link-detect. `common/` services (httpd, mqtt_app, netbios) start on the `net_link` "up" event and
  never know it was Wi-Fi/BLE.
- **`pal_storage`** — the "provisioned" flag (and any app config) uses the same `pal_storage`
  interface; on CC35x1 it's backed by NVS/NVOCMP (OSPI), on TM4C by EEPROM. The WLAN *profile*
  itself is owned by the Wi-Fi stack, not `pal_storage`.

A later, optional `local_ctrl` interface can reuse the same NimBLE host to expose control/status
over BLE when the broker is down — out of scope for provisioning bring-up.

## 6. Bench-validation checklist (when the board is on the desk — Phase 0b/2)

- [ ] Flash the (adapted) provisioning build via LP-XDS110 (USB-C **and** XDS110 to the **same** PC;
      expect >1 min debug launch; **`restart` after load** before `continue`).
- [ ] Board advertises over BLE; SimpleLink Connect sees the service `0xCC00`.
- [ ] Write SSID/security/password, read `0xCC03` → board joins AP; status notifies "connected".
- [ ] Power-cycle → board **auto-reconnects** without re-provisioning (profile persisted in OSPI).
- [ ] Factory-reset trigger clears the profile and re-enters advertising.
- [ ] After connect, the shared web UI is reachable over Wi-Fi and MQTT connects (Phase 2/3 gate).
- [ ] Confirm SysConfig device is CC3551E (demo projectspec targets generic `CC35X1E`); migrate if needed.

## 7. Open questions

- Exact `Wlan_Connect()` + `WlanSecParams_t` signature and the STA connect result events to key the
  LED/`net_link` state machine on (read from `wlan_cmd.c` when we build the adapted app).
- Whether to keep BLE advertising *concurrently* with STA (coexistence) for re-provisioning without
  reboot, or only on the factory-reset trigger (simpler; likely the v1 choice).
- BLE name length/format limits for encoding the client ID.

## 8. Validation results (2026-07-25)

**Toolchain (no board):** Imported `ble_wifi_provisioning_LP_EM_CC35X1_freertos_ticlang.projectspec`
and built via the CCS project MCP: **success**, only `-Ofast` deprecation warnings (harmless). CC35x1
build chain (TICLANG 5.1.1, SysConfig, prebuilt Wi-Fi/BLE/lwIP/FWU libs) works headlessly. Imported
example lives outside git (`.gitignore`d) as a scratch reference until Phase 2 folds an adapted
version into `platform/cc35x1/`.

**Hardware (board connected — full end-to-end PASS):** flashed the demo via XDS110 (no brown-out, no
probe-firmware dialog); board advertised BLE (`Name: TI CC3xxx`, BD `2C:D3:AD:A8:56:CA`); provisioned
from the phone (SimpleLink Connect) over GATT `0xCC00`; board joined the AP (WPA2), ran DHCP, got
**192.168.1.140**, and answered **ping 4/4 (TTL 255)** from the PC on the same LAN. Both halves of the
historical CC35x1 friction (build + flash) cleared. Serial backchannel = COM14 @115200 (UART1 XDS110).
Debug-session note held true: >1 min launch not hit this time, but `restart` after load was required
before `continue`.

**Observed vs. the demo study:** GAP/device name reads `TI CC3xxx` (not `cc35xxble`) — confirm the
exact adv name to encode the client ID later. The "read `0xCC03` triggers connect" behavior held:
logs show `WLAN Security Type Received → Password Received → WLAN CONNECT Requested`.

## 8b. Plan-B result — boot reconnect behavior (2026-07-25, on HW)

**Confirmed: the demo does NOT auto-reconnect on boot.** After power-cycles/resets the board did not
rejoin Wi-Fi (ping `192.168.1.140` → "destination host unreachable"), matching the source: `_app()`
unconditionally advertises BLE and blocks on the credentials sync object — no boot-time connect from a
stored profile. (Serial went flaky mid-session — fine on first flash, silent afterward even on a
healthy `Running` target; CCS/probe wobble after PC reboots — so this was verified by ping + source,
not serial.)

→ **Gate design is therefore decided:** on provisioning success call `Wlan_ProfileAdd(ssid,len,mac,
&WlanSecParams_t,ent,priority,hidden,flags)` (see `wlan_cmd.c:5260`) **and** set the auto-connect
**connection policy** (`Wlan_Set(WLAN_SET_CONNECTION_POLICY, &WlanPolicySetGet_t{autoPolicy=1})`,
`wlan_cmd.c:5105`) — both persist to OSPI. On boot, if a profile exists, skip BLE and let the stack
auto-connect (wait for `WLAN_EVENT_CONNECT`); advertise BLE only when no profile / on factory-reset.

## 9. Next: adapt into a minimal `platform/cc35x1` provisioning module

Turn the demo into our own module (build-testable now, flash-testable on the board):
strip scaffolding (§3) → **provisioned-state gate** (skip BLE when a profile/flag exists) →
button re-provision → **direct `Wlan_Connect()`** (drop `cmd_parser`) → **RGB-LED status** →
client-ID BLE name → on-connect, start the shared `common/` services (NetBIOS + httpd + MQTT) as the
`net_link`-up event.
