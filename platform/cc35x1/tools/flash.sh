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

# --sign-only: re-sign the flash images from the current .out and stop (no probe,
# no programming).  This is the OTA release step -- the resulting
# Debug/toolbox/primary_vendor_image.sign.bin is what you push over the air with
# tools/ota_push.py (or the Tools web page), instead of cold-flashing it.
SIGN_ONLY=0
[ "${1:-}" = "--sign-only" ] && SIGN_ONLY=1

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
OTA_DIR="$BUILD_DIR/ota"     # clean single-file OTA output (one <fingerprint>.bin)

# fw_fingerprint - the YYYYMMDDHHMM build stamp the web UI shows, read from the
# __DATE__/__TIME__ strings baked into buildinfo.o.  Naming the OTA file with this
# makes it match exactly what the device reports once it applies the update.
fw_fingerprint() {
    local o="$BUILD_DIR/buildinfo.o" d t mon day yr hh mm mnum
    d="$(grep -a -o -E '[A-Z][a-z][a-z] [ 0-9][0-9] 20[0-9][0-9]' "$o" 2>/dev/null | head -1)"
    t="$(grep -a -o -E '[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' "$o" 2>/dev/null | head -1)"
    [ -n "$d" ] && [ -n "$t" ] || return 1
    mon="${d:0:3}"; day="${d:4:2}"; yr="${d:7:4}"; hh="${t:0:2}"; mm="${t:3:2}"
    day="${day// /0}"        # space-padded single-digit day -> zero-padded
    case "$mon" in
        Jan) mnum=01;; Feb) mnum=02;; Mar) mnum=03;; Apr) mnum=04;;
        May) mnum=05;; Jun) mnum=06;; Jul) mnum=07;; Aug) mnum=08;;
        Sep) mnum=09;; Oct) mnum=10;; Nov) mnum=11;; Dec) mnum=12;; *) return 1;;
    esac
    printf '%s%s%s%s%s\n' "$yr" "$mnum" "$day" "$hh" "$mm"
}

echo "[flash.sh] Starting CC35x1 flash sequence..."
echo "[flash.sh] Repo: $REPO"
echo "[flash.sh] Build dir: $BUILD_DIR"
echo

[ -x "$TOOLBOX_EXE" ] || { echo "[flash.sh] ERROR: toolbox not found: $TOOLBOX_EXE" >&2; exit 2; }
echo "[flash.sh] ✓ Toolbox found"
[ -f "$BUILD_DIR/mqtt_io_cc35x1.out" ] || { echo "[flash.sh] ERROR: .out missing -- build the CCS project first." >&2; exit 2; }
echo "[flash.sh] ✓ .out file found"
[ -f "$BUILD_DIR/syscfg/action_request_extra.txt" ] || { echo "[flash.sh] ERROR: Debug/syscfg missing -- build the CCS project first." >&2; exit 2; }
echo "[flash.sh] ✓ syscfg artifacts found"
echo

# --- Stage 1: re-sign the flash images from the current .out ------------------
echo "[flash.sh] =========================================="
echo "[flash.sh] Stage 1: Re-signing flash images"
echo "[flash.sh] =========================================="

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

echo "[flash.sh] Invoking toolbox makefile to sign vendor image..."
PATH="/c/ti/ccs2100/ccs/utils/bin:$PATH" \
"$GMAKE" -s -f "$TOOLBOX/scripts/makefile" all \
    SDK_DIR="$SDK" \
    SYSCONFIG_ARTIFACT="$BUILD_DIR_WIN/syscfg" \
    BUILD_DIR="$BUILD_DIR_WIN" \
    BUILD_ARTIFACT="$OUT_WIN" \
    primary_vendor_image_version="$VENDOR_VER" \
    TOOLBOX_DIR="$TOOLBOX" > "$LOG.sign" 2>&1
if [ $? -ne 0 ]; then
    echo "[flash.sh] ERROR: image re-sign failed. Log tail:" >&2; tail -15 "$LOG.sign" >&2; exit 3
fi
echo "[flash.sh] ✓ Image re-signed successfully ($(ls -la --time-style=+%H:%M "$BUILD_DIR/toolbox/primary_vendor_image.sign.bin" | awk '{print $6}') vendor image)"

[ -f "$TOOL_SETTINGS" ] || { echo "[flash.sh] ERROR: $TOOL_SETTINGS missing after re-sign." >&2; exit 3; }
echo "[flash.sh] ✓ Tool settings file ready"
echo

# --- Publish ONE clean OTA artifact: <build-fingerprint>.bin ------------------
# The signed vendor image is the OTA payload.  Copy it to a dedicated Debug/ota/
# dir named with the firmware's build fingerprint (the version the web UI shows),
# and wipe any older artifact so exactly one uploadable file ever sits there.
# NOTE: the toolbox/ intermediates are intentionally LEFT untouched -- the
# cold-flash programmer (Stage 2) consumes the whole signed image set from there.
echo "[flash.sh] Creating OTA artifact directory..."
mkdir -p "$OTA_DIR"
rm -f "$OTA_DIR"/*.bin
echo "[flash.sh] ✓ OTA directory cleaned"

echo "[flash.sh] Extracting firmware build fingerprint from buildinfo..."
if OTA_FP="$(fw_fingerprint)"; then
    OTA_BIN="$OTA_DIR/$OTA_FP.bin"
    echo "[flash.sh] ✓ Build fingerprint: $OTA_FP"
else
    OTA_BIN="$OTA_DIR/primary_vendor_image.sign.bin"
    echo "[flash.sh] ⚠ Could not derive build fingerprint; using generic OTA name" >&2
fi
echo "[flash.sh] Copying signed image to OTA directory..."
cp "$BUILD_DIR/toolbox/primary_vendor_image.sign.bin" "$OTA_BIN"
echo "[flash.sh] ✓ OTA image ready: $OTA_BIN"
echo

if [ "$SIGN_ONLY" -eq 1 ]; then
    echo "[flash.sh] >>> Sign-only mode: OTA image ready (no hardware flashing)"
    echo "[flash.sh] >>> To push over-the-air: python platform/cc35x1/tools/ota_push.py --post <ip> \"$OTA_BIN\""
    exit 0
fi

# --- Stage 2: program via the toolbox 'programmer' ---------------------------
# Probe identity is gated by preflight.sh: it asserts exactly ONE XDS110 and that
# its SN matches the pinned CC35x1 bench probe (CC35_PROBE_SN). This is what makes
# the M4/M33 mix-up impossible on the flash path -- the TM4C's ICDI probe never
# even enumerates here, and a stray second XDS110 hard-fails instead of guessing.
echo "[flash.sh] =========================================="
echo "[flash.sh] Stage 2: Hardware programming"
echo "[flash.sh] =========================================="
echo "[flash.sh] Detecting XDS110 probe and verifying pinned SN..."
source "$HERE/preflight.sh"
SN="$(cc35_probe_sn)" || exit $?
echo "[flash.sh] ✓ Probe detected and verified"

echo "[flash.sh] Checking if built image is fresh..."
cc35_check_out_fresh || true   # stale-.out is a warning, not a flash blocker
echo

echo "[flash.sh] Probe SN: $SN"
echo "[flash.sh] Tool settings: $TOOL_SETTINGS"
echo

# The 'programming' loader inflates the ~1.2 MB vendor image in RAM and can throw
# a transient Python MemoryError under memory pressure -- non-deterministic; retry.
echo "[flash.sh] Starting programming sequence (up to 3 attempts)..."
rc=1
for attempt in 1 2 3; do
    echo "[flash.sh] --- Programming attempt $attempt/3 ---"
    echo "[flash.sh] Invoking toolbox programmer..."
    "$TOOLBOX_EXE" programmer -i XDS110 -param1 "$SN" programming \
        --tool_settings "$TOOL_SETTINGS" \
        --report_file_name_path "$REPORT" \
        --verbose > "$LOG" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "[flash.sh] ✓ Programming succeeded on attempt $attempt"
        break
    fi
    if grep -q "MemoryError" "$LOG"; then
        echo "[flash.sh] ⚠ Transient MemoryError (memory pressure) -- retrying"
        echo "[flash.sh]   Tip: close a browser/Word/CCS tab to free RAM"
        sleep 3
        continue
    fi
    echo "[flash.sh] ✗ Programming FAILED (rc=$rc), not a MemoryError. Log tail:"
    tail -15 "$LOG"
    break
done

echo
if [ "$rc" -eq 0 ]; then
    echo "[flash.sh] =========================================="
    echo "[flash.sh] ✓ FLASH COMPLETE"
    echo "[flash.sh] =========================================="
    echo "[flash.sh] >>> Now USB power-cycle the board for a clean Wi-Fi NWP boot."
else
    echo "[flash.sh] =========================================="
    echo "[flash.sh] ✗ FLASH FAILED (rc=$rc)"
    echo "[flash.sh] =========================================="
    echo "[flash.sh] >>> See detailed log: $LOG"
fi
exit "$rc"
