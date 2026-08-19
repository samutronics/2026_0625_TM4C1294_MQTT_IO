#!/usr/bin/env bash
#
# CC35x1 flash/debug preflight guard -- makes the M4/M33 target mix-up impossible.
#
# Background: on 2026-08-05 a debug launch silently attached to the TM4C `mqtt_io`
# (Cortex-M4) instead of the CC35x1 (Cortex-M33), because CCS `debugProject` uses
# the IDE *active project*, not the probe. It cost real time ("M4 error while CC35
# is M33"). This script encodes the invariants so it can't recur.
#
# Two enforcement surfaces, because flash and debug take different paths:
#
#   FLASH  -- driven here in shell via the XDS110 + toolbox programmer. This script
#             IS the guard: it asserts exactly one XDS110 is present and (optionally)
#             that its SN matches the pinned CC35x1 bench probe. The TM4C uses a
#             Stellaris *ICDI* probe, which `xdsdfu` does not enumerate at all, so an
#             XDS110 match already excludes the TM4C by construction.
#
#   DEBUG  -- driven through the CCS debug MCP / IDE, which shell cannot set. The
#             guard there is the MCP preflight SEQUENCE this script prints (and which
#             CLAUDE.md binds the agent to run): active project must be
#             `mqtt_io_cc35x1`, and after connect `listCores` must show `APP_MCU`.
#
# Usage:
#   preflight.sh              # run all checks, print debug preflight, exit non-zero on hard fail
#   CC35_PROBE_SN=XXXX preflight.sh   # pin an explicit expected probe SN
#   source preflight.sh && cc35_probe_sn   # reuse the detected SN in another script (flash.sh)
#
set -uo pipefail

# The CC35x1 probe SN. Empty (default) accepts any single XDS110. Set CC35_PROBE_SN=<sn>
# to pin a specific probe (e.g., when multiple boards are connected).
CC35_PROBE_SN="${CC35_PROBE_SN:-}"
CC35_PROJECT="mqtt_io_cc35x1"          # the ONLY project that may be active for a CC35x1 debug/flash
CC35_CORE="APP_MCU"                    # the M33 core name that listCores must report

XDSDFU="/c/ti/ccs2100/ccs/ccs_base/common/uscif/xds110/xdsdfu.exe"
_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_REPO="$(cd "$_HERE/../../.." && pwd)"
_OUT="$_REPO/${CC35_PROJECT}/Debug/${CC35_PROJECT}.out"

_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
_ylw()  { printf '\033[33m%s\033[0m\n' "$*"; }

# --- Probe identity: assert exactly one XDS110, matching the pinned SN ----------
# Prints the detected SN to stdout (so callers can `SN=$(cc35_probe_sn)`), and
# diagnostics to stderr. Returns non-zero on any hard failure.
cc35_probe_sn() {
    [ -x "$XDSDFU" ] || { _red "preflight: xdsdfu not found: $XDSDFU" >&2; return 2; }
    local sns count
    sns="$("$XDSDFU" -e 2>/dev/null | grep -i 'Serial Num' | sed 's/.*:[[:space:]]*//' | tr -d '[:space:]\r')"
    count="$(printf '%s\n' "$sns" | grep -c .)"
    if [ "$count" -eq 0 ]; then
        _red "preflight: no XDS110 probe found (xdsdfu -e). Is the CC35x1 plugged in? Replug the USB cable." >&2
        return 2
    fi
    if [ "$count" -gt 1 ]; then
        _red "preflight: multiple XDS110 probes present:" >&2
        printf '  %s\n' $sns >&2
        _red "  Pin one with CC35_PROBE_SN=<sn> so we can't target the wrong board." >&2
        return 2
    fi
    if [ -n "$CC35_PROBE_SN" ] && [ "$sns" != "$CC35_PROBE_SN" ]; then
        _red "preflight: XDS110 SN '$sns' != pinned CC35x1 bench SN '$CC35_PROBE_SN'." >&2
        _red "  Refusing -- this is not the expected CC35x1 probe. Set CC35_PROBE_SN=$sns to override." >&2
        return 2
    fi
    printf '%s' "$sns"
    return 0
}

# --- Stale-image guard: warn (don't fail) if the .out predates any source -------
cc35_check_out_fresh() {
    [ -f "$_OUT" ] || { _red "preflight: .out missing ($_OUT) -- build the CCS project first." >&2; return 3; }
    local newest
    newest="$(find "$_REPO/mqtt_io" "$_REPO/platform/cc35x1" \
                   -type f \( -name '*.c' -o -name '*.h' \) -newer "$_OUT" 2>/dev/null | head -5)"
    if [ -n "$newest" ]; then
        _ylw "preflight: WARNING -- these sources are newer than the .out (stale image?):" >&2
        printf '  %s\n' $newest >&2
        _ylw "  Rebuild (buildProject $CC35_PROJECT) before flashing, or you'll flash old code." >&2
    fi
    return 0
}

# --- Debug-side preflight the agent MUST run (shell can't set IDE state) --------
cc35_print_debug_preflight() {
    cat >&2 <<EOF

  --- CC35x1 DEBUG preflight (run via the CCS MCP before debugProject) ---
  1. getActiveProjectName  ->  MUST equal '$CC35_PROJECT'
       if not: buildProject $CC35_PROJECT (this flips the active project), re-check.
  2. debugProject          ->  (uses the active project verified above)
  3. listCores             ->  MUST report core '$CC35_CORE' (the M33).
       if you see an M4 / TM4C core: terminate, fix the active project, retry.
  Never trust a debug session until step 3 confirms '$CC35_CORE'.
EOF
}

# When executed directly (not sourced), run the full guard.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "=== CC35x1 preflight ==="
    sn="$(cc35_probe_sn)" || exit $?
    _grn "  probe OK: single XDS110, SN $sn${CC35_PROBE_SN:+ (matches pinned $CC35_PROBE_SN)}"
    cc35_check_out_fresh || exit $?
    _grn "  .out present: $_OUT"
    cc35_print_debug_preflight
    echo
    _grn "preflight: flash-safe (probe identity verified)."
fi
