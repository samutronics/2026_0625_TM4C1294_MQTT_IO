# CC3551 PSA FWU — A/B swap requires hardware power-cycle (TI E2E ticket)

**Status:** submitted to TI E2E (2026-09-13). This file preserves the report and
its evidence alongside the code. See also the `cc35x1-ota` engineering notes and
[`platform/cc35x1/webui_platform.c`](../platform/cc35x1/webui_platform.c)
(`OtaPrepareTarget`, `WebPlatformOtaInit`) for the in-firmware handling (graceful
abort + boot-time "power-cycle to apply" detector).

---

**Title:** CC3551 PSA FWU: vendor-image A/B swap does not apply on
`psa_fwu_request_reboot()` when swapping away from a committed slot — requires
hardware power-cycle, then lands in STAGED (not TRIAL) and wedges further OTAs

## Environment
- Device: CC3551 (LP-EM-CC35X1), Cortex-M33 app core
- SDK: SimpleLink Wi-Fi SDK 10.10.01.08
- Firmware update: PSA FWU (`ti/utils/FWU`, `FWU.a`), vendor image A/B slots
  (`Vendor_Image_Slot_1` id 4, `Vendor_Image_Slot_2` id 5)
- Toolchain: tiarmclang 5.1.1.LTS, FreeRTOS
- OTA flow: app streams a signed vendor image → `psa_fwu_start`/`write`/`finish`
  → `psa_fwu_install()` → `psa_fwu_request_reboot()`; on the trial boot, once
  healthy, app calls `psa_fwu_accept()` → `psa_fwu_request_reboot()` to commit.

## Summary
The first OTA after a cold (CLI/toolbox) flash swaps, trials, and commits
correctly using the PSA soft-reboot. The **next** OTA — which must swap *away
from the slot just committed via `psa_fwu_accept()` (COMMIT_AND_PROTECT)* — does
**not** take effect on `psa_fwu_request_reboot()`: the previous image keeps
running and the target slot stays `STAGED`. Only a **hardware power-cycle** boots
the new slot — and it then reports `STAGED` (not `TRIAL`), so `psa_fwu_accept()`
cannot finalize it, leaving it `STAGED`+PRIMARY, which blocks the next
`psa_fwu_install()` with `PSA_ERROR_BAD_STATE (-137)`.

## Expected
Repeated alternating vendor-image OTAs each apply on the
`psa_fwu_request_reboot()` soft reset, boot as `TRIAL`, and commit via
`psa_fwu_accept()` — no manual power-cycle.

## Actual (component states via `psa_fwu_query`)
1. Cold flash → running slot 1 (id4) `UPDATED/PRIMARY`, slot 2 (id5) `READY`.
2. OTA #1 → target slot 2 → `psa_fwu_request_reboot()` → boots new image, slot 2
   = `TRIAL` ✅ → `psa_fwu_accept()` returns `PSA_SUCCESS_REBOOT` → commit →
   slot 2 `UPDATED/PRIMARY`, slot 1 `FAILED`. ✅
3. OTA #2 → target slot 1 → `install()` OK, `psa_fwu_request_reboot()` → **boots
   the OLD image again; slot 1 = `STAGED` (swap did not take).** ❌
4. Hardware power-cycle → boots slot 1, but it reports **`STAGED`+PRIMARY, not
   `TRIAL`** → `psa_fwu_accept()` never fires (requires TRIAL) → stays `STAGED`.
5. Next OTA → `psa_fwu_install()` returns **`-137` (`PSA_ERROR_BAD_STATE`)**
   because a component is left `STAGED`.

### Representative log (step 3)
```
ota: candidate ... -> vendor component 4 ; staged, rebooting into trial   (psa_fwu_request_reboot)
[BOOT] built <OLD timestamp>          <-- old image still running
comp 4 state 3(STAGED)
comp 5 state 7(UPDATED) PRIMARY       <-- committed slot still primary
```

### After HW power-cycle (step 4)
```
comp 4 state 3(STAGED) PRIMARY        <-- swapped, but STAGED not TRIAL
comp 5 state 7(UPDATED)
```

## Analysis
`psa_fwu_request_reboot()` triggers `PRCM_AON__RST_CTRL = 0x1`, which appears to
reset only the M33 core (the NWP keeps running). A full hardware power-cycle
(which also resets the NWP) is the only thing that applies the swap away from a
`COMMIT_AND_PROTECT`ed slot. No application-side mutator can resolve the
resulting stuck state: `psa_fwu_clean()` requires UPDATED/FAILED,
`psa_fwu_cancel()` refuses ACTIVE, and `psa_fwu_reject/accept/request_reboot`
skip the PRIMARY component.

## Questions for TI
1. Is `psa_fwu_request_reboot()` expected to apply a swap **away from a
   `COMMIT_AND_PROTECT`ed slot**, or is a full POR/hardware reset required by
   design? If the latter, is there a supported software-triggerable full reset
   (that also resets the NWP) for use after `psa_fwu_install()`?
2. Why does the power-cycle-applied swap land the target in **`STAGED` instead of
   `TRIAL`**, so `psa_fwu_accept()` cannot finalize it?
3. Is there a supported way to **commit a trial without protecting the slot** (so
   subsequent soft-reboot swaps remain symmetric)?
4. What is the recommended sequence for **repeated, alternating vendor-image
   OTAs** without a manual power-cycle between them?
