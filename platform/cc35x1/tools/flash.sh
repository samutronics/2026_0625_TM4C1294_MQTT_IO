#!/usr/bin/env bash
#
# Cold-flash the CC35x1 over the XDS110 using the SimpleLink WiFi Toolbox
# 'programmer' CLI -- the native CC35XXE secure-flash path (the same programmer
# CCS invokes under the hood), driven directly so no debug session is left open.
#
# Two stages:
#   1. Re-sign the flash images from the CURRENT .out (the toolbox
#      scripts/makefile -- the documented CCS post-build step). This project was
#      created from a projectspec and has NO post-build step wired in, so a bare
#      `buildProject` only produces the .out; without this stage the programmer
#      would flash a STALE primary_vendor_image.sign.bin and your latest code
#      never reaches the board. (Learned the hard way 2026-08-01.)
#   2. Program the freshly-signed image set with the toolbox 'programmer'.
#
# Why not DSLite: CC35X1E flashing is toolbox-driven and needs the CCS *project*
# context (PROJECT_BUILD_DIR) that bare DSLite lacks -> "SetCurrentDirectory
# failed (2)" -> raw fallback connect hits Error -614 (CC35X1E ships
# autoResetOnConnectByDefault=false, so DSLite attaches to the running app whose
# latched SWD sticky-error blocks the connect). The toolbox programmer does its
# own clean connect/reset per image and never wedges the probe.
#
# Prereq: build the CCS project once (compile+link) so Debug/mqtt_io_cc35x1.out
# and Debug/syscfg/ exist. Then just run this script after any rebuild.
#
# After a successful flash: USB power-cycle the board for a clean Wi-Fi NWP boot.
#
set -uo pipefail

SDK="C:/ti/simplelink_wifi_sdk_10_10_01_08"
TOOLBOX="C:/ti/simplelink_wifi_toolbox_win_4_2_4"
TOOLBOX_EXE="/c/ti/simplelink_wifi_toolbox_win_4_2_4/simplelink-wifi-toolbox.exe"
GMAKE="/c/ti/ccs2100/ccs/utils/bin/gmake"
XDSDFU="/c/ti/ccs2100/ccs/ccs_base/common/uscif/xds110/xdsdfu.exe"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REPO_WIN="$(echo "$REPO" | sed 's|^/\([a-zA-Z]\)/|\1:/|')"   # /c/... -> C:/...
BUILD_DIR="$REPO/mqtt_io_cc35x1/Debug"
BUILD_DIR_WIN="$REPO_WIN/mqtt_io_cc35x1/Debug"
OUT_WIN="$BUILD_DIR_WIN/mqtt_io_cc35x1.out"
TOOL_SETTINGS="$BUILD_DIR/toolbox/tool_settings.json"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$HERE/last_programming_report.txt"
LOG="$HERE/flash.log"

[ -x "$TOOLBOX_EXE" ] || { echo "error: toolbox not found: $TOOLBOX_EXE" >&2; exit 2; }
[ -f "$BUILD_DIR/mqtt_io_cc35x1.out" ] || { echo "error: .out missing -- build the CCS project first." >&2; exit 2; }
[ -f "$BUILD_DIR/syscfg/action_request_extra.txt" ] || { echo "error: Debug/syscfg missing -- build the CCS project first." >&2; exit 2; }

# --- Stage 1: re-sign the flash images from the current .out ------------------
echo "=== re-signing flash images from current .out ==="
PATH="/c/ti/ccs2100/ccs/utils/bin:$PATH" \
"$GMAKE" -s -f "$TOOLBOX/scripts/makefile" all \
    SDK_DIR="$SDK" \
    SYSCONFIG_ARTIFACT="$BUILD_DIR_WIN/syscfg" \
    BUILD_DIR="$BUILD_DIR_WIN" \
    BUILD_ARTIFACT="$OUT_WIN" \
    TOOLBOX_DIR="$TOOLBOX" > "$LOG.sign" 2>&1
if [ $? -ne 0 ]; then
    echo "error: image re-sign failed. Tail:" >&2; tail -15 "$LOG.sign" >&2; exit 3
fi
echo "  ok ($(ls -la --time-style=+%H:%M "$BUILD_DIR/toolbox/primary_vendor_image.sign.bin" | awk '{print $6}') vendor image)"

[ -f "$TOOL_SETTINGS" ] || { echo "error: $TOOL_SETTINGS missing after re-sign." >&2; exit 3; }

# --- Stage 2: program via the toolbox 'programmer' ---------------------------
# Probe identity is gated by preflight.sh: it asserts exactly ONE XDS110 and that
# its SN matches the pinned CC35x1 bench probe (CC35_PROBE_SN). This is what makes
# the M4/M33 mix-up impossible on the flash path -- the TM4C's ICDI probe never
# even enumerates here, and a stray second XDS110 hard-fails instead of guessing.
source "$HERE/preflight.sh"
SN="$(cc35_probe_sn)" || exit $?
cc35_check_out_fresh || true   # stale-.out is a warning, not a flash blocker

echo "probe SN: $SN"
echo "settings: $TOOL_SETTINGS"
echo

# The 'programming' loader inflates the ~1.2 MB vendor image in RAM and can throw
# a transient Python MemoryError under memory pressure -- non-deterministic; retry.
rc=1
for attempt in 1 2 3; do
    echo "=== programming attempt $attempt/3 ==="
    "$TOOLBOX_EXE" programmer -i XDS110 -param1 "$SN" programming \
        --tool_settings "$TOOL_SETTINGS" \
        --report_file_name_path "$REPORT" \
        --verbose > "$LOG" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "OK: programming succeeded on attempt $attempt."
        break
    fi
    if grep -q "MemoryError" "$LOG"; then
        echo "  transient MemoryError (memory pressure) -- retrying (close a browser/Word/CCS to free RAM)."
        sleep 3
        continue
    fi
    echo "FAILED (rc=$rc), not a MemoryError. Last lines:"; tail -15 "$LOG"; break
done

echo
if [ "$rc" -eq 0 ]; then
    echo ">>> Now USB power-cycle the board for a clean Wi-Fi NWP boot."
else
    echo ">>> Flash did not complete (rc=$rc). See $LOG."
fi
exit "$rc"
