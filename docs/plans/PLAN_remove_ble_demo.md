# Plan 8 — Decide + remove the BLE provisioning SDK demo project

**Priority:** Low · **Status:** OPEN (needs a roadmap decision first) · Memory: `remove-ble-provisioning-demo-todo`

## Goal
Remove `ble_wifi_provisioning_LP_EM_CC35X1_freertos_ticlang` from the workspace IF BLE provisioning is not on the roadmap. It only declutters — the project is gitignored and re-importable.

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`; memory `remove-ble-provisioning-demo-todo`, `cc35x1-provisioning`.

## Facts (already validated)
- It's an **imported SDK demo**, **gitignored** (0 tracked files → removing it makes no git diff).
- **No code/build dependency** from `mqtt_io` or `mqtt_io_cc35x1` (only two docs mention it: `CC35X1_BLE_PROVISIONING.md`, `CC35X1_PORTING_PROPOSAL.md`).
- **Re-importable** from `C:/ti/simplelink_wifi_sdk_10_10_01_08/examples/rtos/LP_EM_CC35X1/demos/ble_wifi_provisioning/` (+ its `.projectspec`).

## Step 0 — DECISION (ask the user)
Is BLE-based Wi-Fi provisioning still planned (as an alternative to the shipped SoftAP provisioning)?
- **No / deferred** → proceed to removal.
- **Yes** → keep the demo as reference; close this plan without removing.

## Removal steps (only if approved)
1. Ensure it is NOT the CCS **active project** (else `buildProject`/`debugProject` mis-target — see the active-project trap in `CLAUDE.md`). If it is, make `mqtt_io_cc35x1` active first.
2. Remove it from the CCS workspace (project explorer → Remove; do not need "delete from disk" via CCS).
3. Delete the directory `ble_wifi_provisioning_LP_EM_CC35X1_freertos_ticlang/`.
4. **Keep** `CC35X1_BLE_PROVISIONING.md` and `CC35X1_PORTING_PROPOSAL.md` (reference if BLE is revisited).

## Verification
- `mqtt_io` and `mqtt_io_cc35x1` still build green (they never depended on it).
- `git status` shows no change from the deletion (it was gitignored).

## Do NOT
- Do not delete the two reference docs.
- Do not remove it while it is the CCS active project.
