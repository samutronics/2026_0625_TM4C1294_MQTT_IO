# OTA Firmware Upload Failure Debug Plan

**Date:** 2026-08-23  
**Device:** CC35x1 (LP-EM-CC35x1)  
**Issue:** OTA upload consistently fails at byte 48 during PSA vendor image staging with error -133 (manifest parse fail)  
**Scope:** OTA debugging only — do not proceed with Feature 1 (dual WiFi credentials) until resolved

---

## Current State

### What Works
- **Cold-flash via `flash.sh`:** Succeeds, device boots normally
- **Binary validity:** Confirmed — same binary loaded via cold-flash runs without errors (connects to WiFi, gets IP, SNTP syncs, heartbeat running)
- **OTA has worked before:** Feature did work on this device prior to on-demand WiFi scan feature (commit c9ba1e9, 2026-08-22)

### What Fails
- **OTA upload:** Deterministic failure at exact byte 48 of vendor image during manifest parsing
  ```
  ota(post): upload started -> vendor component 5
  ota(post): staging @48 failed (-133), aborting
  ```
- **Error signature:** -133 = PSA FWU manifest parse error (not a connection drop, not a timeout)
- **Reproduced on:** Multiple attempts after factory reset via web interface (IO config cleared, WiFi/MQTT settings preserved)

### Known Changes Since Last Working OTA
1. **On-demand WiFi scan feature** (commits 555-577 in platform/cc35x1/main.c, c9ba1e9)
   - Added `NetWifiScanCache()` call in main tick loop
   - Scan drops and restores STA connection
   - Regenerated `fsdata.c` (binary-embedded HTML filesystem)
   
2. **fsdata.c regeneration** (via `makefsdata.py`)
   - Added "Scan Networks" buttons to `mqtt_io/fs/index.shtml`
   - Ran: `python3 platform/cc35x1/tools/makefsdata.py -o platform/cc35x1/fsdata.c mqtt_io/fs`
   - Synced: `cp platform/cc35x1/fsdata.c mqtt_io_cc35x1/fsdata.c`
   - File is now embedded in the primary_vendor_image

3. **No other code changes to OTA handler or PSA FWU wrapper**

### Hypothesis
The error occurs at byte 48 (vendor image header boundary), suggesting:
- **PSA manifest validator is expecting different header format** (fsdata.c changes shifted binary layout)
- **OR device-side PSA state is corrupted** from NWP boot error (`apGlobal/ifaces NULL`), causing validator to reject valid manifest
- **OR OTA chunk buffer has stale state** from previous failed upload

---

## Diagnostic Steps (In Order)

### Phase 1: Rule Out Binary/Signing Issues
- [x] Cold-flash the binary → boots and runs normally
- [x] Binary is valid (confirmed by execution)
- [x] Conclusion: Issue is NOT firmware corruption or bad signing

### Phase 2: Investigate PSA Manifest Structure
**Goal:** Determine if fsdata.c changes altered the vendor image header format

1. **Compare vendor image headers (old vs new)**
   ```bash
   # Last known working binary (from commit before c9ba1e9):
   # (If available in git history, check it out)
   
   # Current binary:
   xxd mqtt_io_cc35x1/Debug/toolbox/primary_vendor_image.sign.bin | head -5
   ```
   Look for: total image size, magic numbers, version fields in first 64 bytes

2. **Verify fsdata.c is correctly embedded**
   - Byte size of old fsdata.c vs new fsdata.c
   - If new is significantly larger, it may have shifted other sections
   - Check: `ls -l mqtt_io_cc35x1/Debug/toolbox/primary_vendor_image.sign.bin` (should be ~1.27 MB)

3. **Check manifest in the signed image**
   - SimpleLink WiFi SDK PSA FWU manifest is embedded after the code image
   - Byte 48 might be in the manifest header — error -133 suggests manifest validation failed
   - Extract and inspect the manifest structure (tool: SimpleLink WiFi Toolbox or hex dump)

### Phase 3: Investigate Device-Side PSA State
**Goal:** Determine if NWP boot error corrupted PSA firmware state

1. **Boot sequence inspection**
   - Device boots with: `ERROR ! apGlobal or apGlobal->ifaces is NULL`
   - This error comes from SimpleLink WiFi driver, not our code
   - **Does this error appear on the old working build?** (Check git history for log)
   - If this is a NEW error introduced by our changes, it's the smoking gun

2. **PSA state reset via power-cycle**
   - Already tested (user did cold-flash, which implied power-cycle)
   - If OTA still fails after power-cycle, PSA state is not the root cause

3. **PSA FWU state inspection** (if available)
   - Check if device-side OTA handler can report PSA manifest state
   - Look for device-side logs at OTA staging time (byte 48)
   - Serial console should show more detail than just error code

### Phase 4: OTA Handler Code Review
**Files to examine:**
- `platform/cc35x1/ota_*.c` — OTA staging, manifest validation, chunk buffering
- `mqtt_io_cc35x1/*.c` — If there's an OTA handler wrapper
- SimpleLink WiFi SDK `simplelink_wifi_sdk_*/source/ti/net/ota/` — PSA FWU wrapper

**What to look for:**
- Manifest validation at byte 48 boundary
- Chunk buffer alignment (is staging offset correct?)
- State between consecutive OTA attempts (does state persist from failed attempt?)
- Error -133 source and what triggers it

### Phase 5: Isolate fsdata.c as the Culprit
**Goal:** Confirm whether fsdata.c changes broke OTA

1. **Revert fsdata.c to pre-scan version**
   - Rebuild with old fsdata.c
   - Cold-flash and verify it boots
   - Try OTA with old fsdata.c binary
   - If OTA succeeds → fsdata.c changes broke it
   - If OTA still fails → something else changed

2. **Rebuild with minimal fsdata.c change**
   - Instead of regenerating entire fsdata.c, just add the button HTML separately
   - Rebuild, test OTA
   - Compare binary size/structure vs full regenerate

### Phase 6: Compare with SDK Examples
**Goal:** Verify PSA FWU manifest structure is correct

1. **Check SimpleLink WiFi SDK for OTA examples**
   - Path: `C:/ti/simplelink_wifi_sdk_10_10_01_08/examples/` (or similar)
   - Look for working OTA example with firmware update
   - Compare manifest generation against our toolchain

2. **Verify toolbox sign/manifest generation**
   - `platform/cc35x1/tools/flash.sh` invokes: `gmake -f simplelink_wifi_toolbox_win_4_2_4/scripts/makefile`
   - Check if this makefile has OTA-specific flags or manifest options
   - Are there --sign-only output differences that affect PSA manifest?

---

## Test Commands

### Verify binary being uploaded
```bash
ls -lh mqtt_io_cc35x1/Debug/ota/
# Expected: 202608231045.bin (or latest fingerprint)
```

### OTA upload with verbose output
```bash
python platform/cc35x1/tools/ota_push.py --post 192.168.1.140 mqtt_io_cc35x1/Debug/ota/202608231045.bin --verbose
```

### Inspect vendor image header
```bash
xxd -l 256 mqtt_io_cc35x1/Debug/toolbox/primary_vendor_image.sign.bin
# Look for magic numbers, size fields, version in first 64 bytes
```

### Compare with old working build (if in git history)
```bash
git show c9ba1e8:mqtt_io_cc35x1/fsdata.c > /tmp/fsdata_old.c
# (commit before c9ba1e9)
ls -l mqtt_io_cc35x1/fsdata.c /tmp/fsdata_old.c
```

---

## Rollback Plan (If fsdata.c is the Culprit)

1. **Revert scan feature fsdata.c changes**
   ```bash
   git checkout c9ba1e8 -- mqtt_io_cc35x1/fsdata.c platform/cc35x1/fsdata.c mqtt_io/fs/index.shtml
   ```

2. **Rebuild**
   - In CCS: Build `mqtt_io_cc35x1` project

3. **Test OTA**
   - If it succeeds, fsdata.c is the issue
   - Next step: Re-add HTML changes in a way that doesn't break PSA manifest

---

## Success Criteria

- [ ] OTA upload completes without error -133
- [ ] Device receives firmware and applies update
- [ ] After reboot, device runs new firmware (verify via heartbeat timestamp or feature presence)
- [ ] Subsequent OTA uploads work (not a one-time fluke)

---

## Context for Next Session

**Start a new conversation with:**
1. Copy this file into the thread
2. Run **Phase 1, Phase 2.1** diagnostics first (quick hex dumps + file size comparison)
3. Based on findings, proceed to Phase 3 or Phase 4
4. If fsdata.c is confirmed as culprit, work on a fix that preserves scan feature + fixes OTA

**Do NOT:**
- Implement Feature 1 (dual WiFi credentials) until OTA is working
- Make other code changes until this is isolated
- Proceed with next commits until OTA is resolved

