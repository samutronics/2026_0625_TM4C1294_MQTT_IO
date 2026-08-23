# OTA Fix Plan — PSA FWU version bump (error -133 / NOT_PERMITTED)

**Date:** 2026-08-23
**Device:** CC35x1 (LP-EM-CC35x1)
**For:** implementer (smaller model) — this plan is self-contained; do not redesign it, execute it.
**Prereq reading before editing:** `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` (mandatory before any CCS/TI MCP tool), repo `CLAUDE.md`, and this file. Confirm you have read them, then start Part A.

---

## 1. Root cause (already diagnosed — do NOT re-investigate)

OTA uploads fail **deterministically at byte 48 with `-133`**:
```
ota(post): upload started -> vendor component 5
ota(post): staging @48 failed (-133), aborting
```
- `-133` = **`PSA_ERROR_NOT_PERMITTED`** (a PSA policy rejection — NOT a parse error, NOT a connection drop, NOT corruption).
- Byte 48 = the detached-manifest boundary. `TI_FWU_MANIFEST_SIZE = sizeof(PSA_FWU_GPESlot_t)` = **48 bytes**. The staging core calls `psa_fwu_start(g_target, manifest, 48)` exactly when the stream reaches offset 48 — see [platform/cc35x1/webui_platform.c:331-337](platform/cc35x1/webui_platform.c#L331). So the failure **is `psa_fwu_start()` rejecting the manifest by policy.**
- The policy is **anti-downgrade**: PSA FWU rejects a candidate whose version is **not strictly greater** than the currently-installed image.

**Why it happens here:** the vendor-image version is **static**. It comes from the SysConfig-generated file
`mqtt_io_cc35x1/Debug/syscfg/action_request_extra.txt` →
```
primary_vendor_image_version=0.0.17.0
```
which the toolbox makefile consumes at `--version "$(primary_vendor_image_version)"`
(`C:/ti/simplelink_wifi_toolbox_win_4_2_4/scripts/makefile:133`, sourced via `include` at line 15).
Every build is signed `0.0.17.0`. Once the device is running `0.0.17.0` (which it is, from repeated cold-flashes), every OTA of `0.0.17.0` is `candidate == active` → **NOT_PERMITTED**.

**Evidence this is correct (don't re-litigate):** cold-flash of the *same* signed image always works (the toolbox programmer bypasses `psa_fwu_start`'s version check); the pre-scan revert also fails at byte 48 (content-independent); OTA "worked before" only when the active image was an older version. `OtaPrepareTarget()` already cancels/cleans stale slots ([webui_platform.c:201-216](platform/cc35x1/webui_platform.c#L201)), so device state is NOT the issue.

---

## 2. The fix (three parts, do in order)

- **Part A** — make `flash.sh` sign each image with a **monotonically increasing** version (overriding the static syscfg value on the make command line). This is both the confirmation test and the permanent fix.
- **Part B** — device-side logging so `-133` is never misdiagnosed again.
- **Part C** — update the debug docs and commit.

Do NOT edit the `.syscfg` for this (leave `primary_vendor_image_version = "0.0.17.0"` as the floor). The make override wins over the `include`d value; no SysConfig MCP change is needed.

---

## Part A — Auto-bump the OTA image version in `flash.sh`

**File:** `platform/cc35x1/tools/flash.sh`

### A1. Add a version-picker block BEFORE the gmake sign call
The gmake invocation is the `"$GMAKE" -s -f "$TOOLBOX/scripts/makefile" all \ ...` block (around line 90, in "Stage 1"). Immediately **above** the `echo "[flash.sh] Invoking toolbox makefile..."` line, insert:

```bash
# --- OTA image version (PSA FWU anti-downgrade) -------------------------------
# The signed vendor image carries a version in its 48-byte GPE-slot manifest.
# On the device, psa_fwu_start() REJECTS any candidate whose version is not
# strictly greater than the installed image -> PSA_ERROR_NOT_PERMITTED (-133).
# The SysConfig default (action_request_extra.txt) is static 0.0.17.0, so every
# build was identical and every OTA after the first was rejected. We override it
# on the make command line (CLI assignment beats the makefile's include) with a
# value that increments on every sign, so each image is strictly newer.
#
# Version format MUST keep the 3rd field ("patch") as the counter so it stays
# ABOVE the installed 0.0.17.0 (compared field-by-field: 0.0.18.0 > 0.0.17.0).
# Counter ceiling is 255 (each field is a byte); bump manually past that.
VER_FILE="$HERE/ota_version.txt"
if [ -n "${OTA_IMAGE_VERSION:-}" ]; then
    VENDOR_VER="$OTA_IMAGE_VERSION"                 # explicit override, e.g. first bench test
else
    n="$(cat "$VER_FILE" 2>/dev/null)"; n="${n:-17}" # last counter; seed 17 (== installed)
    n=$((n + 1))                                     # strictly greater than installed
    if [ "$n" -gt 255 ]; then
        echo "[flash.sh] ERROR: OTA version counter > 255; bump the minor field manually in flash.sh." >&2
        exit 3
    fi
    VENDOR_VER="0.0.$n.0"
    echo "$n" > "$VER_FILE"
fi
echo "[flash.sh] OTA image version: $VENDOR_VER (overrides syscfg default)"
```

### A2. Pass the override to gmake
In the existing gmake call, add one variable assignment line. Change:
```bash
"$GMAKE" -s -f "$TOOLBOX/scripts/makefile" all \
    SDK_DIR="$SDK" \
    SYSCONFIG_ARTIFACT="$BUILD_DIR_WIN/syscfg" \
    BUILD_DIR="$BUILD_DIR_WIN" \
    BUILD_ARTIFACT="$OUT_WIN" \
    TOOLBOX_DIR="$TOOLBOX" > "$LOG.sign" 2>&1
```
to (add the `primary_vendor_image_version=...` line):
```bash
"$GMAKE" -s -f "$TOOLBOX/scripts/makefile" all \
    SDK_DIR="$SDK" \
    SYSCONFIG_ARTIFACT="$BUILD_DIR_WIN/syscfg" \
    BUILD_DIR="$BUILD_DIR_WIN" \
    BUILD_ARTIFACT="$OUT_WIN" \
    primary_vendor_image_version="$VENDOR_VER" \
    TOOLBOX_DIR="$TOOLBOX" > "$LOG.sign" 2>&1
```

### A3. Seed the counter file
Create `platform/cc35x1/tools/ota_version.txt` containing a single line:
```
17
```
(First real sign will bump it to 18 → version `0.0.18.0`, strictly greater than the installed `0.0.17.0`.)
Add this file to git. If the device is already known to be on a version **higher** than 0.0.17.0, seed the counter to that field value instead (see Part B log to read the device's active version first).

### A4. `bash -n` the script
Run `bash -n platform/cc35x1/tools/flash.sh` — must print nothing (syntax OK).

---

## Part B — Device-side logging (so -133 is unambiguous next time)

**File:** `platform/cc35x1/webui_platform.c`

### B1. Add a PSA status-name helper (near the top, after the includes / helper section)
```c
// Human-readable name for the PSA status codes we actually hit on the OTA path.
// Keeps the -133 mystery from recurring (it is NOT a parse error; it is policy).
static const char *
PsaStatusName(psa_status_t st)
{
    switch(st)
    {
        case PSA_SUCCESS:                 return("SUCCESS");
        case PSA_ERROR_NOT_PERMITTED:     return("NOT_PERMITTED (version/anti-downgrade)");
        case PSA_ERROR_INVALID_SIGNATURE: return("INVALID_SIGNATURE");
        case PSA_ERROR_INVALID_ARGUMENT:  return("INVALID_ARGUMENT");
        case PSA_ERROR_DATA_INVALID:      return("DATA_INVALID");
        case PSA_ERROR_DATA_CORRUPT:      return("DATA_CORRUPT");
        case PSA_ERROR_BAD_STATE:         return("BAD_STATE");
        case PSA_ERROR_STORAGE_FAILURE:   return("STORAGE_FAILURE");
        default:                          return("(other)");
    }
}
```
(If any of those enum names don't exist in `ti/utils/FWU/psa_fwu.h` / the pulled-in `psa/error.h`, drop that case. Verify names in the header before compiling.)

### B2. Enrich the two "staging @… failed" logs
Find the POST staging error log (~line 753) and the chunk-CGI one (~line 530). Change each from the current form:
```c
PalLog("ota(post): staging @%u failed (%d), aborting\n",
       (unsigned)g_szFileOffset, (int)st);
```
to include the decoded name:
```c
PalLog("ota(post): staging @%u failed (%d = %s), aborting\n",
       (unsigned)g_szFileOffset, (int)st, PsaStatusName(st));
```
(Match the exact existing variable names/format in each spot; only add `= %s` + `PsaStatusName(st)`.)

### B3. Log the active image version at OTA start and at boot
The candidate-vs-active version mismatch is the whole story, so print the **active** version.
1. In `OtaPrepareTarget()` ([webui_platform.c:165](platform/cc35x1/webui_platform.c#L165)), after it has picked `target` and queried the slots, log the **active** (primary) slot's version. `psa_fwu_component_info_t` has a version field — check its exact name/type in `psa_fwu.h` (likely `.version` of type `psa_fwu_image_version_t` with `.major/.minor/.patch/.build` or similar). Add:
```c
// sInfoActive = the currently-running (non-target) slot's info, already queried above.
PalLog("ota: active image version %u.%u.%u.%u; candidate must be strictly greater\n",
       sInfoActive.version.major, sInfoActive.version.minor,
       sInfoActive.version.patch, sInfoActive.version.build);
```
Adapt field names to the real struct. If the struct shape is unclear, at minimum log the raw bytes.
2. In `WebPlatformOtaInit()` ([webui_platform.c:842](platform/cc35x1/webui_platform.c#L842)), add the same active-version log once at boot so it shows in the heartbeat/boot log.

### B4. Two-copy sync check
`webui_platform.c` is a **linked resource** (no `mqtt_io_cc35x1/` copy) — edit the canonical file only. (Confirm: there is no `mqtt_io_cc35x1/webui_platform.c`.) Do NOT copy it.

---

## Part C — Docs + commit

### C1. Correct `OTA_DEBUG_RESULTS.md`
Append a "RESOLVED" section stating: root cause = static `primary_vendor_image_version=0.0.17.0` → `psa_fwu_start()` returns `PSA_ERROR_NOT_PERMITTED (-133)` on any equal/older candidate; cold-flash bypasses the check. The earlier hypotheses (manifest-format break, SDK/toolbox regression, PSA state corruption, chip-erase) are **refuted**. Fix = monotonic version bump in `flash.sh`.

### C2. Commit (commits go straight to `main` per repo workflow)
Stage exactly: `platform/cc35x1/tools/flash.sh`, `platform/cc35x1/tools/ota_version.txt`, `platform/cc35x1/webui_platform.c`, `OTA_DEBUG_RESULTS.md`, and this `OTA_FIX_PLAN.md`.
Commit message (end with the trailer):
```
cc35x1: fix OTA -133 by bumping PSA FWU image version per build

psa_fwu_start() rejected every OTA with PSA_ERROR_NOT_PERMITTED (-133)
because the vendor image was always signed 0.0.17.0 (static syscfg value)
== the installed version, tripping PSA's anti-downgrade policy. flash.sh
now overrides primary_vendor_image_version with a monotonic counter so
each signed image is strictly newer. Adds device-side PSA status/version
logging. Cold-flash was unaffected (it bypasses psa_fwu_start).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```
Do NOT push until the bench test (below) passes. Ask the user before `git push`.

---

## 3. Build → sign → flash/OTA (exact commands)

The build (compile+link → `.out`) happens in **CCS**, not the CLI. Sequence:

1. **Build** `mqtt_io_cc35x1` in CCS (or via the ccs-project MCP `buildProject mqtt_io_cc35x1`) so `mqtt_io_cc35x1/Debug/mqtt_io_cc35x1.out` and `Debug/syscfg/` are fresh. Confirm green.
2. **Sign only** (creates the versioned signed image, no hardware) — from the repo root in Git Bash:
   ```bash
   platform/cc35x1/tools/flash.sh --sign-only
   ```
   Watch for: `[flash.sh] OTA image version: 0.0.18.0` and `Debug/ota/<fingerprint>.bin` produced.
   Sanity-check the version actually reached the toolbox: `grep -i version platform/cc35x1/tools/flash.log.sign` should not show it falling back to 0.0.17.0.
3. **Confirm the device's current active version** (read the CC35x1 serial console, COM14, via pyserial per the repo memory `serial-use-pyserial` — NOT the ccs-serial MCP). Look for the new `ota: active image version …` boot log. Ensure your signed version (0.0.18.0) is strictly greater. If the device is already ≥ 0.0.18.0, re-seed `ota_version.txt` higher and re-sign.
4. **OTA push** the freshly signed image:
   ```bash
   python platform/cc35x1/tools/ota_push.py --post <device_ip> mqtt_io_cc35x1/Debug/ota/<fingerprint>.bin --verbose
   ```
   Expected device log now: `ota: manifest accepted, streaming image` (instead of `staging @48 failed`).
5. After the upload completes and the device stages + reboots, **USB power-cycle** the board (a warm reboot can wedge the Wi-Fi NWP — see repo memory `cc35x1-nwp-reset`). Confirm it boots the new image (heartbeat/build fingerprint or the bumped active version in the boot log).

---

## 4. Success criteria

- [ ] `flash.sh --sign-only` logs `OTA image version: 0.0.18.0` and the toolbox used it (not 0.0.17.0).
- [ ] OTA upload runs past byte 48 — device logs `ota: manifest accepted, streaming image`, no `-133`.
- [ ] Upload completes, device stages, reboots (after USB power-cycle) into the new image.
- [ ] A **second** OTA (version auto-bumps to 0.0.19.0) also succeeds — proves the monotonic bump works build-over-build, not just once.
- [ ] Boot log shows the active image version.

---

## 5. Rollback (if something regresses)

- The change is isolated to `flash.sh` + `ota_version.txt` + logging in `webui_platform.c`. To revert signing behavior: `git checkout -- platform/cc35x1/tools/flash.sh platform/cc35x1/tools/ota_version.txt`. Cold-flash (`flash.sh` full run) still recovers the board regardless.
- If an OTA half-applies, cold-flash via the toolbox programmer (full `flash.sh`) + USB power-cycle restores a known-good image (it ignores the version policy).

---

## 6. Do NOT

- Do NOT hand-edit `mqtt_io_cc35x1.syscfg` (CCS.md forbids it; and the make override makes it unnecessary).
- Do NOT chase "manifest format", "SDK/toolbox downgrade", "chip erase", or "PSA state corruption" — all refuted; cold-flash of the same image proves the image + signing are valid.
- Do NOT touch Feature 1 (dual Wi-Fi creds) or any unrelated code in this task.
- Do NOT `git push` before the two-OTA bench test passes; ask the user first.
- Do NOT use the ccs-serial MCP for COM14 — use raw pyserial (repo memory `serial-use-pyserial`).
- Do NOT warm-reset the board via the debugger to "reboot" it — USB power-cycle only.

---

## 7. Files touched (summary)

| File | Change | Linkage |
|------|--------|---------|
| `platform/cc35x1/tools/flash.sh` | version-picker block + `primary_vendor_image_version=` on the gmake line | script |
| `platform/cc35x1/tools/ota_version.txt` | new — monotonic counter, seed `17` | new file |
| `platform/cc35x1/webui_platform.c` | `PsaStatusName()`, enrich staging-fail logs, log active version at start+boot | linked (edit canonical only, no copy) |
| `mqtt_io_cc35x1.syscfg` | **unchanged** (floor stays 0.0.17.0) | — |

> Note: this file supersedes the old `OTA_DEBUG_PLAN.md` / `OTA_DEBUG_RESULTS.md`
> (removed — their raw test data lives in git commit `468bcc6`; their conclusions
> were wrong and are corrected in section 1 above).

---

## 8. Handoff kickoff prompt (paste as the first message to the implementer model)

Start a **new conversation** in this same workspace, select a smaller model, and paste:

```
Read OTA_FIX_PLAN.md at the repo root, then C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md,
the repo CLAUDE.md, and the memory index
C:/Users/tomik/.claude/projects/c--Users-tomik-Workspaces-Workspace2026-0625-TM4C1294-MQTT-IO/memory/MEMORY.md.
Confirm you have read them.

Then execute OTA_FIX_PLAN.md ONE part at a time, in order:
  - Part A first (flash.sh version auto-bump + ota_version.txt seed 17).
    After Part A: run `bash -n platform/cc35x1/tools/flash.sh` (must be clean),
    then `platform/cc35x1/tools/flash.sh --sign-only` and confirm it logs
    "OTA image version: 0.0.18.0" and that flash.log.sign shows the toolbox
    used 0.0.18.0 (not 0.0.17.0). STOP and report.
  - Part B only after Part A is confirmed (verify psa_fwu_component_info_t field
    names in ti/utils/FWU/psa_fwu.h before compiling).
  - Part C (docs + commit) last.

Hard rules:
  - Do NOT flash hardware or git push — the bench flash/OTA test and the two-OTA
    verification are done by the user (or a hardware-capable session).
  - Do NOT hand-edit the .syscfg; the make-command-line override is the fix.
  - Do NOT re-investigate the root cause — it is proven in section 1. Do NOT chase
    manifest format / SDK downgrade / chip erase.
  - webui_platform.c is a linked resource: edit the canonical file only, no copy.
  - Build (compile+link) is done in CCS / via the ccs-project MCP, not the CLI.
```
