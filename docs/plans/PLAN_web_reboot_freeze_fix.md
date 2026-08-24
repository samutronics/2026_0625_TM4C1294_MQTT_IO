# Plan 1 — Fix CC35x1 web-UI Reboot SW freeze

**Priority:** High · **Status:** OPEN · Memory: `cc35x1-web-reboot-freeze`

## Goal
Make "Reboot" from the web UI actually reboot the CC35x1 and come back up, OR remove the button if a clean app-initiated reboot is impossible. Today it freezes: UART shows `Reboot triggered via web UI.` then the device hangs.

## Before you start (read)
- `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md`, repo `CLAUDE.md`, memory `MEMORY.md`.
- Memories: `cc35x1-web-reboot-freeze`, `cc35x1-nwp-reset`, `cc35x1-provisioning`.

## Root cause (already diagnosed — do not re-investigate)
`/reboot.cgi` → `RebootCGIHandler` logs the line at `mqtt_io/common/webui.c:596` → `main.c:326` calls `WebPlatformFinalizeReboot()` → `PalReboot()` = Cortex‑M33 `SCB->AIRCR` SYSRESETREQ (`platform/cc35x1/webui_platform.c:976`). A warm SYSRESETREQ leaves the Wi‑Fi **NWP half‑wedged** (DHCP may work, TCP dead → hang). Only a USB power‑cycle boots the NWP cleanly. This is the SAME hazard as `cc35x1-nwp-reset`. PSA‑OTA reboots are clean because the bootloader re‑inits the NWP; a bare app `PalReboot()` does not.

## Approach — try in this order, stop at the first that verifies on HW
1. **Clean NWP shutdown before reset.** In the CC35x1 `PalReboot()` path (`platform/cc35x1/pal_sys.c` and/or `WebPlatformFinalizeReboot` in `webui_platform.c`), before `SCB->AIRCR`, bring Wi‑Fi down cleanly: role‑down / `Wlan_Stop` (see how `NetWifiStaDown`/`NetWifiApDown` tear down in `platform/cc35x1/net_wifi.c`), then a short delay, then SYSRESETREQ. **Likely insufficient** per `cc35x1-nwp-reset` (PRCM CONNSTP reset already runs each `Wlan_Start`) — but verify on HW.
2. **If (1) still wedges:** accept that app‑initiated reset cannot re‑init the NWP. Change the web Reboot to the **safe alternative**: either (a) remove the Reboot button from `mqtt_io/fs/*.shtml` + disable `/reboot.cgi` on CC35x1 (keep on TM4C via the platform seam), or (b) repurpose it to a "Wi‑Fi restart" that does a live `NetWifiStaDown()`→`NetWifiStaUp()` (no chip reset) if that satisfies the user's need.
3. Whatever path wins, keep TM4C behavior unchanged (TM4C reboot is fine).

## Files likely touched
- `platform/cc35x1/pal_sys.c` (PalReboot), `platform/cc35x1/webui_platform.c` (WebPlatformFinalizeReboot),
- possibly `mqtt_io/common/webui.c` / `webui.h` (seam to disable/relabel reboot per platform),
- possibly `mqtt_io/fs/*.shtml` (+ regenerate both `fsdata.c` copies — see Plan 6/two‑copy gotcha).

## Verification (HW)
- Flash (`platform/cc35x1/tools/flash.sh` full run) → **USB power‑cycle**.
- Trigger Reboot from the web UI. PASS = device resets AND rejoins Wi‑Fi (web reachable, MQTT online) with no freeze, no manual power‑cycle. If it does NOT rejoin, path (1) failed → go to path (2).
- Capture COM14 (pyserial) across the reboot.

## Do NOT
- Do not warm‑reset via the debugger to "test" it (wedges NWP, false negative).
- Do not touch the Wi‑Fi F3 watchdog here — but note the outcome informs Plan 3 (if reboot can't be made clean, F3 must use live‑retry, not `PalReboot()`).

## Commit
Branch off `main` if needed; commit with a clear message + `Co-Authored-By` trailer. Ask before push.
