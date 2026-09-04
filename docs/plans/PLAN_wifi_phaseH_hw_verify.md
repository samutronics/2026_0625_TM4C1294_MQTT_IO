# Plan 3 — Wi-Fi Phase H hardware verification (dual creds + display + AP watchdog)

**Priority:** High · **Status:** OPEN (code complete, unverified) · Memory: `cc35x1-provisioning`

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
