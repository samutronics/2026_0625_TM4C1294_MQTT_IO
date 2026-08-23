# OTA Debug Test Results
**Date:** 2026-08-23  
**Device:** CC35x1 (LP-EM-CC35x1)  

## Test Progression

### Test 1: Current Binary (with WiFi scan feature)
- **Binary size:** 1,262,600 bytes
- **Failure mode:** PSA manifest parse error -133 at byte 48
- **Root cause:** Likely fsdata.c changes altered binary layout
- **Status:** ❌ CRITICAL FAILURE (0.004% complete)

### Test 2: fsdata.c Reverted Only  
- **Binary size:** Reverted to pre-scan fsdata.c
- **Failure mode:** Connection aborted at chunk 212/309
- **Progress:** 68.6% before device dropped connection
- **Finding:** fsdata.c IS part of the issue, but not the complete cause
- **Status:** ❌ PARTIAL PROGRESS - manifest parsing fixed, new failure point

### Test 3: Fully Reverted (to commit 9b5fec2)
- **Binary size:** 1,256,412 bytes
- **Failure mode:** Connection aborted at chunk 59/307  
- **Progress:** 19.2% before device dropped connection
- **Finding:** Earlier failure suggests device state NOT clean between tests
- **Status:** ❌ REGRESSION - suspect PSA FWU state corrupted from failed attempts

## Key Observations

1. **fsdata.c changes ARE a culprit:** Reverting improved manifest parsing (byte 48 → chunk 212)
2. **Code changes (main.c, webui) made it worse:** Fully reverted version fails even earlier
3. **Device PSA state may be corrupted:** Different failures suggest stale state between OTA attempts
4. **No clean success path yet:** Even pre-scan version fails OTA

## Next Steps (Priority Order)

### Phase 1: Clean Device State
- [ ] Power-cycle device via XDS110 board reset
- [ ] Clear all PSA FWU state if possible  
- [ ] Test OTA again with pre-scan binary

### Phase 2: Investigate PSA State
- [ ] Check if SimpleLink WiFi SDK has PSA state reset utilities
- [ ] Look for PSA manifest corruption indicators in device logs
- [ ] Compare with working SDK examples

### Phase 3: Binary Size Analysis  
- [ ] Check if larger binary exceeds PSA OTA buffer  
- [ ] Verify PSA manifest structure in signed image
- [ ] Compare vendor image headers (working vs broken)

### Phase 4: Root Cause per Code Path
**If Test 1 (clean state) succeeds with pre-scan:**
- WiFi scan feature + fsdata.c changes broke OTA
- Refactor feature to avoid binary layout changes

**If Test 1 still fails:**
- Deeper PSA/platform issue unrelated to scan feature
- May require SDK update or hardware workaround

---

## Session 2: Pre-Scan Code Testing (2026-08-23 15:22)

### Clean Device Test with Fully Reverted Code (9b5fec2)
- **Reverted:** all files to pre-scan commit (webui.c, config.h, fsdata.c, main.c, net_wifi.c, webui_platform.c, wifi_store.c/h)
- **Flashed:** Clean cold-flash via flash.sh
- **Device boot:** Normal, WiFi connected at 192.168.1.140, MQTT active
- **OTA Result:** **STILL FAILS at byte 48 with -133** (manifest parse error)
- **Key finding:** Pre-scan code failure indicates root cause is NOT the WiFi scan feature or fsdata changes

## Critical Discovery

**The pre-scan binary (9b5fec2) ALSO fails OTA at byte 48.** This shifts the diagnosis:

- ❌ NOT caused by WiFi scan feature (c9ba1e9)
- ❌ NOT caused by fsdata.c regeneration
- ✓ Likely caused by: PSA FWU state corruption, toolchain change, or SDK incompatibility

### Binary Analysis
OTA binary structure (202608231439.bin):
- Bytes 0-47: Manifest (PSA FWU format with signatures/hashes)
- Byte 48+: Image data (0xFFFF padding)
- The device's PSA validator rejects the manifest at byte 48

### Hypothesis
Since even the known-good pre-scan code fails OTA, either:
1. Device PSA state is fundamentally corrupted (requires full chip erase)
2. Toolbox/SDK version mismatch has broken manifest format
3. Signing process changed and manifest is now invalid

## Context for Next Session
**Root cause isolation required:**
- [ ] Compare manifest bytes between working vs broken OTA (if historical binary available)
- [ ] Full chip erase + bootloader reprogram to clear all PSA state
- [ ] SDK/toolbox version check and potential upgrade/downgrade
- [ ] Investigate SimpleLink WiFi SDK PSA FWU manifest format and validation
