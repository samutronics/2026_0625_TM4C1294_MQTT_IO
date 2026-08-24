# Plan 2 — Fix CC35x1 boot-time Wi-Fi connect race (clean the boot log)

**Priority:** Low (cosmetic/robustness) · **Status:** OPEN · Memory: `cc35x1-boot-connect-race-todo`

## Goal
Stop the first STA connect from firing before the NWP is ready, so the boot log no longer shows the error burst and the first attempt succeeds (saving one ~12 s retry).

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`.
- Memories: `cc35x1-boot-connect-race-todo`, `cc35x1-provisioning`, `cc35x1-ota`, `cc35x1-nwp-reset`.

## Symptom (observed UART)
```
ERROR ! apGlobal or apGlobal->ifaces is NULL   net: connecting to ozike3 (sec 2)
CME :... CmeStationFlowSmValidateTransitionUserEvent ...
CME :CCmeStationFlowSM: ERROR! UnExpected event ...
net: disconnected (reason 15, initiator 0)
net: no IP after attempt 1/3, reconnecting  -> attempt 2 connects -> IP 192.168.1.140
```
`reason 15` = 4‑way handshake timeout. The `apGlobal ... NULL` line is a known‑benign NWP warning (see `cc35x1-ota`) but here it means we call `Wlan_Connect()` before the NWP finished IniParams.

## Approach
In `platform/cc35x1/main.c`, in the boot sequence between `NetWifiDriverStart()` (Wlan_Start) and the first `NetWifiStaUp()`/connect (around the boot STA‑up block, ~lines 155‑200), insert a **wait for NWP/STA readiness** before the first connect:
- Preferred: wait for a definitive readiness signal — inspect `platform/cc35x1/net_wifi.c` for a WLAN "provisioning/started/ready" event or a queryable state (e.g. an `apGlobal`/interface‑ready check the SDK exposes). Add a bounded poll (e.g. up to ~1‑2 s) that returns as soon as ready.
- Fallback if no clean signal exists: a bounded delay after `NetWifiDriverStart()` returns (measure the smallest delay on HW that removes the first‑attempt failure).
- Keep the existing 3‑attempt retry as the safety net — this change should make attempt 1 succeed, not replace the retry.
- Optional: downgrade/quieten the known‑benign NWP boot warnings in `PalLog` output if they still appear.

## Files
- `platform/cc35x1/main.c` (boot STA‑up block), possibly a small helper in `platform/cc35x1/net_wifi.c` (`NetWifiWaitReady()`), `net_wifi.h`.

## Verification (HW)
- Flash + USB power‑cycle; capture COM14 (pyserial) from reset.
- PASS = first connect succeeds: no `reason 15`, no `no IP after attempt 1/3`, `net: IP …` appears after attempt 1; time‑to‑IP drops by ~one attempt. The `apGlobal NULL` line may still print harmlessly before the connect — acceptable, but connect must not fail.

## Do NOT
- Do not remove or weaken the 3‑attempt retry.
- Do not add an unbounded/very long delay — keep it minimal and event‑driven if possible.

## Commit
Clear message + `Co-Authored-By` trailer; ask before push.
