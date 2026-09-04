# Plan 5 — Make OTA/PSA version use the FW build-timestamp nomenclature

**Priority:** Med · **Status:** OPEN · Memory: `ota-version-match-fw-timestamp-todo`

## Goal
Replace the hand-maintained OTA counter (`0.0.37.0` from `ota_version.txt`) with a version **derived from the FW build timestamp** (`YYYYMMDDHHMM`), so OTA and FW use one nomenclature and there is no separate counter to maintain. Delete `ota_version.txt`.

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`.
- Memories: `ota-version-match-fw-timestamp-todo`, `cc35x1-ota`, `build-tooling`.
- Root doc `OTA_FIX_PLAN.md` (the current version-override mechanism this plan modifies).

## Hard constraint (verified)
PSA version struct (`C:/ti/simplelink_wifi_sdk_10_10_01_08/source/ti/utils/FWU/psa_fwu.h:106`):
```c
typedef struct { uint8_t iv_major; uint8_t iv_minor; uint16_t iv_revision; uint32_t iv_build_num; } PSA_FWU_GPEVersion_t;
```
So `--version "A.B.C.D"` = major(u8).minor(u8).**revision(u16)**.**build(u32)**. A full 12‑digit `YYYYMMDDHHMM` (~2.0e11) overflows even the u32 build field — it cannot go in one field.

## Design (encode timestamp, display as timestamp)
- Split the build stamp: **`iv_revision = YYMM`** (u16, ≤ 9912) and **`iv_build_num = DDHHMM`** (u32, ≤ 312359). Keep `major.minor = 0.0`. Example: build `202608241530` → PSA version `0.0.2608.241530`.
- **Monotonic** under PSA field‑wise compare (major→minor→revision→build): YYMM rises across months/years; within a month DDHHMM rises (DD most significant). So each build is strictly newer → no `-133`.
- **Display:** wherever the OTA version is shown to a human, reconstruct and print it as the timestamp `YYYYMMDDHHMM` (i.e. `20{revision}{build zero‑padded to 6}`), matching the FW revision string exactly.

## Steps
1. **`platform/cc35x1/tools/flash.sh`** — in the version block that the OTA fix added (currently reads `ota_version.txt` and builds `VENDOR_VER="0.0.$n.0"`):
   - Reuse the existing `fw_fingerprint()` (already extracts `YYYYMMDDHHMM` from `buildinfo.o`). Derive `YYMM="${FP:2:4}"` and `DDHHMM="${FP:6:6}"`. Set `VENDOR_VER="0.0.$YYMM.$DDHHMM"`.
   - Keep the `OTA_IMAGE_VERSION=` env override for manual cases.
   - **Delete** `platform/cc35x1/tools/ota_version.txt` and the counter read/write logic.
   - Pass `primary_vendor_image_version="$VENDOR_VER"` to gmake exactly as today.
2. **Device‑side display** — `platform/cc35x1/webui_platform.c`: the active‑version log added by the OTA fix (`ota: active image version …`) should print the reconstructed timestamp `20{iv_revision}{iv_build_num:06}` (keep the raw fields too if helpful). Update the web UI OTA/`fwver` rendering similarly so the operator sees the same string as the FW revision.
3. Update `OTA_FIX_PLAN.md` (or its archived copy) to note the counter was replaced by the timestamp-derived version.

## Verification (HW)
- `flash.sh --sign-only` → confirm log `OTA image version: 0.0.<YYMM>.<DDHHMM>` and `flash.log.sign` shows the toolbox used it.
- Two successive builds (different minutes) → OTA both; second must be accepted (strictly greater), device logs the reconstructed timestamp as the active version. No `-133`.

## Edge cases / Do NOT
- Two builds within the SAME minute → equal version → PSA rejects. Rare; if hit, force a fresh `buildinfo` (`touch mqtt_io/buildinfo.c`) or bump build by 1.
- Do NOT hand-edit `.syscfg` (the make override wins; leave the syscfg default as floor).
- `YYMM` scheme is monotonic to year 2099 — fine.

## Commit
Clear message + `Co-Authored-By`; ask before push. Verify the CC35x1 build + a real OTA before pushing.
