# Plan 3 — Wi-Fi Phase H hardware verification (dual creds + display + AP watchdog)

**Priority:** High · **Status:** DONE (2026-09-14, HW-verified) · Memory: `cc35x1-provisioning`

## Outcome (2026-09-14)
All Phase H features HW-verified on the CC3551 bench:
- **F1** dual creds + RSSI-ranked join ✅, cascade to backup ✅, both-fail → setup AP ✅.
- **F2** both saved SSIDs displayed, masked with a working Show toggle, backup save-only
  (no link drop) ✅.
- **F3** — implemented as the **live AP fallback (no `PalReboot`)**, which is what this plan
  prescribed as the alternative if the reboot-watchdog proved unsafe (it does — see
  `cc35x1-web-reboot-freeze`). Forget clears both slots and brings up the setup AP **live**;
  a bug where the live STA→AP switch after tearing down an associated link failed
  (`Wlan_RoleUp(AP) -2147482582`, wedged until HW reset) was fixed with a bounded
  `Wlan_RoleUp(AP)` retry+settle (commit `7cff9dc`), HW-verified.
- Compile-time seed changed to a tracked non-joining placeholder `dummyAP`/`dummyPSW`
  (commit `15a9543`) so an emptied store falls through to the setup AP instead of silently
  rejoining a real bench network.

Not adopted: the original F3 **reboot** watchdog (`PalReboot` on AP timeout) — it shares the
NWP-wedge hazard, so the live-retry fallback is the shipping behavior.

## Goal
Verify on hardware the three already-committed Wi-Fi Phase H features, and — the make-or-break item — confirm the F3 AP-reboot watchdog actually recovers Wi-Fi after `PalReboot()`. If it does not, switch F3 to the live AP→STA retry.

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`.
- **Spec doc `WIFI_FEATURES_PLAN.md`** (same folder; the full feature spec + its own verification section) — this plan is the execution wrapper around it.
- Memories: `cc35x1-provisioning`, `cc35x1-web-reboot-freeze` (F3 shares that hazard), `cc35x1-nwp-reset`, `serial-use-pyserial`.

## What to verify (from WIFI_FEATURES_PLAN.md §Verification)
1. Build `mqtt_io_cc35x1` green (ccs-project MCP `buildProject`) AND confirm the TM4C `mqtt_io` gmake build still green (shared files + stubs).
2. Flash (`platform/cc35x1/tools/flash.sh`) → **USB power-cycle**. Capture COM14 via pyserial.
3. **F1 dual creds / RSSI:** save two networks (primary + backup) in Settings→Wi-Fi; reboot → log shows a scan and that it joined the **stronger** SSID. Power off the stronger AP, reboot → joins the other. Both wrong → stays in setup AP.
4. **F2 display:** Settings→Wi-Fi shows both saved SSIDs; password masked with a working "Show" toggle.
5. **F3 watchdog (CRITICAL):** with creds saved, disable the AP so the node falls to setup AP; confirm the log `wifi: AP fallback 5 min, rebooting…` at ~5 min, the board resets, **and Wi-Fi rejoins after the reboot** (re-enable the AP first). "Forget" → setup AP and must NOT auto-reboot.

## The make-or-break check
Because web-UI reboot freezes via the same `PalReboot()` NWP-wedge (see `cc35x1-web-reboot-freeze` / Plan 1), F3 is at high risk of freezing too. If after the 5-min reboot the device does **not** rejoin Wi-Fi:
- Switch F3 to the **live AP→STA retry** (no chip reset): in `platform/cc35x1/main.c` `NetWifiIsAp()` branch, replace the `PalReboot()` at `AP_REBOOT_MS` with: `NetWifiApDown()` → `NetWifiScanCache()` → `NetWifiStaUp(pcSsid,pcPass)` and arm the existing 45 s no-IP fallback so it returns to AP if the retry fails. Re-verify.

## Files (only if F3 needs the live-retry rework)
- `platform/cc35x1/main.c` (F3 branch). No other files unless Plan 1 changes the reboot seam.

## Do NOT
- Do not warm-reset via debugger (false results). USB power-cycle only.
- Do not start other tasks; this is verification + at most the F3 rework.

## Output
Update `WIFI_FEATURES_PLAN.md` status (verified / or F3 switched to live-retry) and the `cc35x1-provisioning` memory. Commit any F3 change with a clear message + trailer; ask before push.
