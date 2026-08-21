# Plan: Dual Wi-Fi credentials + saved-network display + AP-mode reboot watchdog (CC35x1)

## Status — TASK CLOSED (code complete, pending commit + HW verification)

**Last Updated:** 2026-08-21

### Completed
- ✅ **On-demand WiFi scan** (prerequisite, not in original plan) — commit `c9ba1e9`
  - `/wifiscan.cgi` + `WebUIRequestWifiScan()`; "Scan Networks" button on both WiFi sections;
    `wifi_scanning.html` spinner w/ 7 s auto-refresh; safe STA teardown→re-up with saved creds;
    RSSI logged over UART; works in AP (provisioning) and STA (connected) modes.
- ✅ **Feature 1 — Dual credentials + RSSI-ranked join** (committed: `main.c`)
  - `wifi_store` API now slot-based (`WifiStoreLoad/Save/Clear(int iSlot, …)`), `WIFI_STORE_SLOTS 2`,
    address table `{CFG_WIFI_EEPROM_ADDR, CFG_WIFI2_EEPROM_ADDR}` + extended `_Static_assert`s.
  - `main.c`: `wifi_rank_candidates()` (line ~137), boot flow ranks both slots and joins the
    stronger, cascades to the other, then provisioning AP if both fail.
- ✅ **Feature 2 — Show saved SSID + masked/revealable password** (committed: `webui.c/.h`, `index.shtml`)
  - SSI tags `wssid1/wpass1/wssid2/wpass2` → `WebPlatformWifiSsid/Pass()` seam; primary + backup
    (`/wificfg.cgi?slot=1`) forms prefill from store; TM4C stubs in `enet_io.c`.
- ✅ **Feature 3 — AP-mode 5-minute reboot watchdog** (committed: `main.c`)
  - `AP_REBOOT_MS` = 5 min; `ui32ApRebootMs` accumulator in the `NetWifiIsAp()` branch,
    guarded by `bHaveCreds` (never reboots the provisioning AP); reset on IP-acquired / provision / forget.

### ⚠️ Uncommitted working-tree changes that complete the build
HEAD (`534dcb8`) references the new slot-based API + `WebPlatformWifiSsid/Pass` whose **definitions
live only in the working tree** — the tree must be committed for a coherent build:
`platform/cc35x1/wifi_store.c` / `.h`, `platform/cc35x1/webui_platform.c`, `mqtt_io/enet_io.c`,
`mqtt_io/config.h` (adds `CFG_WIFI2_EEPROM_ADDR 4720u`).
The `mqtt_io_cc35x1/wifi_store.c` copy already matches (line-ending diff only — benign).

### Open TODOs (see bottom of file)
Commit the tree → build both projects → flash + HW-verify the 6 checks → confirm F3 NWP recovery.

---

## Context
Today the CC35x1 node stores **one** Wi-Fi credential set ([wifi_store.c](platform/cc35x1/wifi_store.c),
single EEPROM record at `CFG_WIFI_EEPROM_ADDR`), the Settings page never shows what network is
saved (fields start blank — [index.shtml](mqtt_io/fs/index.shtml) `set-wifi` pane), and when the
home router is down the node falls to the setup AP and **stays there forever** (main.c AP branch is
"nothing to retry"). Three improvements requested:

1. **Two SSID/PSW slots**, join the **stronger** (RSSI) at boot; only if *both* fail → provisioning AP.
2. **Show the saved network name + password** (password masked, reveal toggle) on the web page.
3. **AP-mode watchdog**: if it fell to AP because the router was down, **reboot after 5 min** to
   retry the preset Wi-Fi (so the node self-heals once the router returns).

Everything reuses infrastructure that already exists: the RSSI-sorted scan cache
(`NetWifiScanCache`/`NetWifiScanCount`/`NetWifiScanGet` in [net_wifi.c](platform/cc35x1/net_wifi.c)),
the magic+CRC32 EEPROM record pattern, the SSI-tag + platform-seam pattern in
[webui.c](mqtt_io/common/webui.c), and `PalReboot()` ([pal_sys.c](platform/cc35x1/pal_sys.c)).

Scope: **CC35x1 only**; TM4C gets empty stubs for the two new web seams so the shared files still build.

---

## Feature 1 — Two credential slots, RSSI-ranked join

### 1a. `wifi_store.c` / `.h` — parameterize by slot
- `#define WIFI_STORE_SLOTS 2`.
- Change the three APIs to take a slot index (update the ~3 callers):
  `bool WifiStoreLoad(int iSlot, char *pcSsid, char *pcPass);`
  `bool WifiStoreSave(int iSlot, const char *pcSsid, const char *pcPass);`
  `void WifiStoreClear(int iSlot);`
- Internal address table `{ CFG_WIFI_EEPROM_ADDR, CFG_WIFI2_EEPROM_ADDR }`; bounds-check `iSlot`.
- [config.h](mqtt_io/config.h): add `CFG_WIFI2_EEPROM_ADDR 4720u` (after slot 1 which ends at 4716;
  4-byte aligned, well under the 6144 EEPROM end). Extend the `_Static_assert`s in `wifi_store.c`
  to cover slot 1 (`CFG_WIFI2_EEPROM_ADDR + sizeof(tWifiRecord) <= 6144`) and non-overlap with slot 0.

### 1b. Candidate ranking helper — in [main.c](platform/cc35x1/main.c) (orchestrator; keeps `wifi_store` pure storage and `net_wifi` pure driver)
```c
typedef struct { char ssid[WIFI_SSID_MAX+1]; char pass[WIFI_PASS_MAX+1];
                 int8_t rssi; bool present; bool valid; } tWifiCand;
static int wifi_rank_candidates(tWifiCand *pC);   // returns count of valid slots
```
- Load both slots via `WifiStoreLoad(i,…)`; keep the existing compile-time `WIFI_SSID`/`WIFI_PASS`
  dev fallback as slot-0 when no stored slot is valid.
- `validCount <= 1` → return in slot order, **skip the scan** (preserves today's fast single-cred boot).
- `validCount == 2` → `NetWifiScanCache()` once, then for each slot look up its SSID via
  `NetWifiScanCount()`/`NetWifiScanGet()` to fill `present`+`rssi`; sort **present-first, then RSSI
  desc** (slot order as tiebreak). This is the "join the stronger" rule.

### 1c. Boot flow rewrite — [main.c](platform/cc35x1/main.c) ~155-207
Replace the single-cred block with: rank candidates → if `nCand==0` start provisioning AP
(`bHaveCreds=false`); else iterate candidates in ranked order, for each `NetWifiStaUp(ssid,pass)` +
the existing 3×12 s IP-poll/`NetWifiReconnect` retry, `break` on IP, else `NetWifiStaDown()` and try
the next. If none join → `NetWifiScanCache()`+`NetWifiApUp()` (fallback AP, `bHaveCreds` stays true).
The winning candidate's creds stay in `pcSsid`/`pcPass` for the runtime reconnect loop (unchanged).

---

## Feature 2 — Show saved SSID + masked/revealable password

### 2a. Platform seam ([webui.h](mqtt_io/common/webui.h) + [webui_platform.c](platform/cc35x1/webui_platform.c))
`int WebPlatformWifiSsid(int iSlot, char *pcBuf, int iLen);`
`int WebPlatformWifiPass(int iSlot, char *pcBuf, int iLen);`
- CC35x1: `WifiStoreLoad(iSlot,…)` then copy SSID/pass **HTML-attribute-escaped** (`&`,`"`,`<`,`>`);
  add a small `WebUIHtmlAttrEscape()` in webui.c if no escaper exists yet, and reuse it.
- TM4C stub ([enet_io.c](mqtt_io/enet_io.c), where the other `WebPlatform*` stubs live): return 0/empty.

### 2b. SSI tags — [webui.c](mqtt_io/common/webui.c) `g_pcConfigSSITags` + `SSIHandler`
Add `wssid1`, `wpass1`, `wssid2`, `wpass2` (≤8 chars) → call the seam above.

### 2c. [index.shtml](mqtt_io/fs/index.shtml) `set-wifi` pane
- Prefill slot-1 fields: `value="<!--#wssid1-->"`, password field `type="password"
  value="<!--#wpass1-->"` with a small "Show" checkbox toggling the input `type` (mask by default).
- Add a **second** network form (slot 2, backup) with its own `/wificfg.cgi?slot=1` submit and
  `<!--#wssid2-->`/`<!--#wpass2-->`. Note: password is present in page HTML so the toggle can reveal
  it — acceptable for a LAN device per the chosen option; document this inline.
- Regenerate **both** `fsdata.c` copies with [tools/makefsdata.py](platform/cc35x1/tools/makefsdata.py)
  (`platform/cc35x1/fsdata.c` + `mqtt_io_cc35x1/fsdata.c` — the two-copy gotcha).

### 2d. Provisioning CGI — [webui.c](mqtt_io/common/webui.c) `WifiCfgCGIHandler`
Parse an optional `slot` param (default 0), carry it through the pending-flag path
(`WebUIRequestWifiProvision`/`WebUIWifiProvisionPending` gain a slot arg) so the main-loop provision
handler calls `WifiStoreSave(slot,…)`. Slot 0 still triggers the live AP→STA switch; **slot 1 saves
only** (backup network — no immediate switch), so today's provisioning UX is unchanged.

---

## Feature 3 — AP-mode 5-minute reboot watchdog

In the runtime loop's `NetWifiIsAp()` branch ([main.c](platform/cc35x1/main.c) ~395), which is
currently a no-op:
```c
if (NetWifiIsAp()) {
    if (bHaveCreds) {                 // fell to AP because join failed (router down) — self-heal
        ui32ApRebootMs += SYSTICKMS;
        if (ui32ApRebootMs >= AP_REBOOT_MS) {   // #define AP_REBOOT_MS (5U*60U*1000U)
            PalLog("wifi: AP fallback 5 min, rebooting to retry preset Wi-Fi\n");
            vTaskDelay(pdMS_TO_TICKS(50));
            PalReboot();
        }
    } else {
        ui32ApRebootMs = 0;           // provisioning AP (no creds) — never auto-reboot
    }
}
```
- Reset `ui32ApRebootMs = 0` on IP-acquired and when a provision/forget request is handled.
- In the **forget** handler set `bHaveCreds = false` so the intentional setup-AP is never rebooted.
- Guard chosen deliberately: the watchdog only fires in the *fallback* AP (creds exist), not while a
  user is provisioning. Reboot uses `PalReboot()`; boot then re-runs Feature 1's ranked join.

---

## Source edit checklist (exact files, verified linkage)
Linkage was checked on disk: files listed as **COPY** exist in BOTH `platform/cc35x1/` and
`mqtt_io_cc35x1/` and CCS compiles the copy — edit the canonical file then copy it over. Files listed
as **linked** have no `mqtt_io_cc35x1/` duplicate and compile from the canonical path — edit once.

| # | File | Feature(s) | What to change | Sync? |
|---|------|-----------|----------------|-------|
| 1 | [platform/cc35x1/wifi_store.h](platform/cc35x1/wifi_store.h) | F1 | Add `WIFI_STORE_SLOTS 2`; add `int iSlot` param to `WifiStoreLoad/Save/Clear` | **COPY → `mqtt_io_cc35x1/wifi_store.h`** if a copy exists (header may be include-only; verify) |
| 2 | [platform/cc35x1/wifi_store.c](platform/cc35x1/wifi_store.c) | F1 | Address table `{4608, CFG_WIFI2_EEPROM_ADDR}`; slot bounds-check; extend `_Static_assert`s | **COPY → `mqtt_io_cc35x1/wifi_store.c`** (copy confirmed on disk) |
| 3 | [mqtt_io/config.h](mqtt_io/config.h) | F1 | Add `#define CFG_WIFI2_EEPROM_ADDR 4720u` | linked (shared) |
| 4 | [platform/cc35x1/main.c](platform/cc35x1/main.c) | F1, F3 | `wifi_rank_candidates()` helper + `tWifiCand`; boot-flow rewrite (~155-207); AP reboot watchdog in the `NetWifiIsAp()` branch (~395); reset counter on IP/provision; `bHaveCreds=false` in forget path | linked (no copy) |
| 5 | [mqtt_io/common/webui.h](mqtt_io/common/webui.h) | F2 | Declare `WebPlatformWifiSsid/Pass(int,char*,int)`; add `slot` arg to `WebUIRequestWifiProvision`/`WebUIWifiProvisionPending` | linked (shared) |
| 6 | [mqtt_io/common/webui.c](mqtt_io/common/webui.c) | F2 | Add SSI tags `wssid1/wpass1/wssid2/wpass2` + `SSIHandler` cases; `WebUIHtmlAttrEscape()` helper if none; `slot` param in `WifiCfgCGIHandler` + pending-flag plumbing | linked (shared) |
| 7 | [platform/cc35x1/webui_platform.c](platform/cc35x1/webui_platform.c) | F2 | Implement `WebPlatformWifiSsid/Pass` via `WifiStoreLoad(iSlot,…)` + escape | linked (no copy) |
| 8 | [mqtt_io/enet_io.c](mqtt_io/enet_io.c) | F2 | TM4C stubs for `WebPlatformWifiSsid/Pass` → return 0 (confirm this is where existing `WebPlatform*` TM4C stubs live) | linked (TM4C build) |
| 9 | [mqtt_io/fs/index.shtml](mqtt_io/fs/index.shtml) | F2 | Prefill slot-1 fields; add slot-2 backup form (`/wificfg.cgi?slot=1`); "Show password" toggle | source for #10 |
| 10 | `platform/cc35x1/fsdata.c` **and** `mqtt_io_cc35x1/fsdata.c` | F2 | Regenerate from #9 via `tools/makefsdata.py`; **both copies** | **COPY (two-copy gotcha)** |

Not edited (reused as-is): `net_wifi.c` (scan/STA/AP APIs already sufficient), `pal_sys.c`
(`PalReboot`), `mqtt_app.c`, all `din_chain`/`relay_chain`/output code.

## Handoff to a cheaper Claude model (e.g. Sonnet) — how to delegate implementation
This plan is deliberately decomposed so a cheaper model can execute it feature-by-feature without
re-discovering the codebase. To hand off:

1. **Point it at this plan file first**, then require it to read three things before editing:
   `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` (mandatory per repo CLAUDE.md), the repo
   `CLAUDE.md`, and the memory index `…/memory/MEMORY.md` (the two-copy + NWP-reset gotchas).
2. **Give it the table above verbatim** as its work list and tell it to implement **one feature at a
   time in order F1 → F2 → F3**, building (`buildProject mqtt_io_cc35x1`) after each feature and not
   proceeding until green. Small, verifiable steps keep a cheaper model on-rails.
3. **Pin the exact anchors** it needs (already in this plan): `WifiStoreLoad/Save/Clear` signatures,
   `NetWifiScanCount/Get`, `g_pcConfigSSITags`/`SSIHandler`, `WifiCfgCGIHandler`, the main.c boot
   block (~155-207) and AP branch (~395), and `PalReboot()`. Tell it **not to invent new APIs** —
   every primitive it needs already exists.
4. **State the three hard rules** it must not violate (these are the usual regressions):
   - After editing `wifi_store.*` or regenerating `fsdata.c`, **copy the file into `mqtt_io_cc35x1/`**
     (rows 1, 2, 10) or CCS compiles the stale copy.
   - Any main-loop tick that may MQTT-publish must run under `LOCK_TCPIP_CORE()` (existing rule).
   - The AP reboot watchdog must be guarded by `bHaveCreds` (never reboot the provisioning AP).
5. **Have it stop at flashing** — do the `flash.sh` + USB-power-cycle + on-HW verification (steps
   1-6) yourself or on a model with hardware-tool access; the cheaper model produces the diff and a
   green build only.

A concrete kickoff prompt for the cheaper model:
> "Read `WIFI_FEATURES_PLAN.md`, then `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md`, the repo
> `CLAUDE.md`, and `…/memory/MEMORY.md`. Implement **Feature 1 only** from the Source edit checklist
> (rows 1-4). Do not touch F2/F3 files yet. Follow the two-copy sync rule for `wifi_store.c`. When
> done, run `buildProject mqtt_io_cc35x1` and report the result — do not flash."

## Reuse (don't reinvent)
- `NetWifiScanCache/Count/Get` — already RSSI-sorted + deduped; the ranking helper just reads it.
- Existing 3×12 s boot retry + 45 s no-IP fallback + `NetWifiSwitchToSta` — unchanged.
- Magic+CRC32 record + `_Static_assert` layout pattern — slot 1 is a second instance of `tWifiRecord`.
- SSI-tag + `WebPlatform*` seam pattern; `PalReboot()` for the watchdog.

## Risks / caveats
- **NWP warm-reboot hazard**: memory notes a software reset can leave the Wi-Fi NWP wedged
  (USB power-cycle recovers). You chose the hard-reboot watchdog — **verification step 4 confirms on
  HW that Wi-Fi actually comes back after `PalReboot()`**; if it wedges, switch Feature 3 to the
  live AP→STA retry (drop AP → rescan → `NetWifiStaUp`, reusing the 45 s fallback to restore AP).
- **Password in page source**: revealable password ⇒ present in HTML; acceptable per chosen option,
  documented inline.
- **+8 s boot** only when *two* creds are stored (scan needed to rank); single-cred boot unchanged.
- Two-copy `fsdata.c`; shared `webui.c`/`config.h` must still compile the TM4C `mqtt_io` gmake build.

---

## Session Summary: On-Demand WiFi Scan (Prerequisite)

The scan feature added in this session enables users to refresh available networks mid-session
without rebooting, which is essential for Feature 1 (dual credentials with RSSI ranking). Users
can now click "Scan Networks" to see updated network list before selecting which AP to connect to.

### Implementation Details

| File | Change | Commit |
|------|--------|--------|
| `mqtt_io/common/webui.h` | Add `WebUIRequestWifiScan()` / `WebUIWifiScanPending()` declarations | c9ba1e9 |
| `mqtt_io/common/webui.c` | Add `/wifiscan.cgi` handler + request/pending functions | c9ba1e9 |
| `mqtt_io/fs/index.shtml` | Add "Scan Networks" button to Primary section (backup already had one) | c9ba1e9 |
| `mqtt_io/fs/wifi_scanning.html` | New file: spinner page with 7s auto-refresh to settings | c9ba1e9 |
| `platform/cc35x1/main.c` | Add scan request handler: check AP/STA mode, call NetWifiScanCache(), auto-reconnect in STA | c9ba1e9 |
| `platform/cc35x1/net_wifi.c` | Add UART logging of each discovered SSID + RSSI (per-network level) | c9ba1e9 |
| `platform/cc35x1/fsdata.c` + `mqtt_io_cc35x1/fsdata.c` | Regenerate to embed updated HTML with scan buttons | c9ba1e9 |

### Behavior

**AP Mode (Provisioning):**
- Scan runs immediately, populates dropdown with discovered networks

**STA Mode (Connected):**
- Scan disconnects temporarily, finds networks, then **reconnects to saved AP**
- Resets MQTT and IP flags to re-establish broker connection
- No user intervention needed; connection is seamless

**UART Log Example:**
```
wifi: user scan requested (STA mode - will disconnect/reconnect during scan)
net: scan found SSID 'HomeNet' (RSSI -45)
net: scan found SSID 'Guest' (RSSI -62)
net: scan complete, 2 network(s) cached
wifi: reconnecting to 'HomeNet' after scan
```

### Why This Helps Feature 1

The dual-credential feature will need to display the strongest available AP in the dropdown
so the user can see which network the device will prefer. The scan-on-demand feature makes
this user-friendly: users can refresh the list anytime, and selecting an AP updates the
dropdown in real-time.

---

## Verification (HW, COM14 via pyserial; flash via `flash.sh`)
1. `buildProject mqtt_io_cc35x1` green; confirm TM4C `mqtt_io` gmake build still green (shared edits + stubs).
2. Save two networks (slot 1 primary, slot 2 backup) in Settings → Wi-Fi; reboot → COM14 log shows the
   scan + that it joined the **stronger** SSID. Power off the stronger AP, reboot → joins the other.
3. Settings → Wi-Fi shows both saved SSIDs; password masked, "Show" reveals it.
4. Router-down test: with creds saved, disable the AP so it falls to setup AP; confirm COM14 logs
   "AP fallback 5 min, rebooting…" at ~5 min, the board resets, **and Wi-Fi rejoins after reboot**
   (re-enable the AP first). If Wi-Fi does *not* come back post-reboot, revert Feature 3 to live-retry.
5. Both creds wrong → after all attempts it stays in setup AP; "Forget" returns to setup AP and does
   **not** auto-reboot.
6. Confirm MQTT, relays, inputs, OTA unaffected.

---

## OPEN TODOs (to fully close)
1. **Commit the working tree** — `wifi_store.c/.h`, `webui_platform.c`, `enet_io.c`, `config.h`
   (these hold the definitions HEAD already calls; build is incoherent until committed).
   `CLAUDE_WORKFLOW.md` remains intentionally untracked.
2. **Build both projects green** — `buildProject mqtt_io_cc35x1` (M33) **and** the TM4C `mqtt_io`
   gmake build (shared `webui.c`/`config.h` + `enet_io.c` stubs must still compile/link).
3. **Flash + on-HW verification** (COM14 via pyserial; `flash.sh`; USB power-cycle, not a debugger
   reset) — run verification steps 1–6 above.
4. **Confirm F3 NWP recovery (highest risk)** — after the 5-min `PalReboot()`, Wi-Fi must actually
   rejoin. If the NWP wedges (per `cc35x1-nwp-reset` memory), switch F3 to the live AP→STA retry
   documented in Risks/caveats.
5. **RSSI-rank correctness** — with two saved SSIDs, verify it joins the stronger and cascades to the
   other when the stronger is powered off.
6. **Optional cleanup** — normalize `wifi_store.c` line endings (canonical vs `mqtt_io_cc35x1/` copy
   differ only by CRLF/LF); regenerate `fsdata.c` only if `index.shtml` changes again.
