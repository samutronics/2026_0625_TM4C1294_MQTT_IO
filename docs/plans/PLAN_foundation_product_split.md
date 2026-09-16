# Plan 11 — Split into reusable IoT foundation + home-automation product

**Status:** IN PROGRESS (design approved 2026-09-14. Early hooks: `product_api.h` +
`product_poll()`/`product_init()`, commits `34ea6bd`/`6155172`/`d424333`. **The three-file
in-place cleave is now COMPLETE** (2026-09-15): `mqtt_app` → `product_on_connect`/`product_on_mqtt`
(`9983517`); `webui` CGI+SSI table → `product_web_register`/`product_ssi_handler` (`e17f7f8`+`ecec2f8`);
`config` store → `product_config_load`/`product_config_factory_reset` (`5c8bbb0`). Both MCUs build
green; CC35x1 web/OTA + factory-reset HW-verified. **The CCS-closed directory rename (Scope A) is
now DONE** (2026-09-16, `f3c5f45`): `git mv mqtt_io_common → iot_foundation` (51 renames, history
preserved) + 7 path-fixup files; CC35x1 generated `.project`/`.cproject` hand-edited in place (no
reimport, build steps preserved). Both MCUs build green after reopen (had to clear stale `Debug/*.d`
dep files pinning the old path). **NEXT: Scope B/C** — `git mv` the whole-file product TUs into
`products/home_auto/`, then split the dual-half `mqtt_app`/`webui`/`config` PRODUCT sections into
separate `products/home_auto/…` TUs (Scope C adds new .c files → real projectspec/.project surgery).)
**Area:** Structure **Priority:** Med

## Execution model guidance (pick per phase)

This is CCS project surgery (renameProject moves dirs; projectspec reimport drops build steps;
two-copy fsdata gotcha), not mechanical text editing — **do not use Haiku.** Default driver is
**Sonnet / medium effort**; escalate to **Opus** for the two judgment-heavy phases. Whatever the
model: read `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` first (CLAUDE.md rule), build green via the
ccs-project MCP after every change set, and never hand-edit `.project`/`.cproject`/`.syscfg`.

| Phase | Model / effort |
|---|---|
| 0 — worktree spike (finalize per-file split, prove CCS wiring) | **Opus / high** — design risk lives here (cgifuncs + board-glue split, linked-resource setup) |
| 1–2 — rename + carve foundation, demo projects | Sonnet / medium |
| 3 — home-auto as product (`product_api.h` hook wiring, restore behavior) | **Opus / medium–high** |
| 4 — HW verify (TM4C + CC35x1) | Sonnet / medium |
| 5–6 — emeter stub + docs | Sonnet / low–medium |

Single-model fallback: **Sonnet / medium** for the whole job (plan is detailed, build gate is the
safety net) — but review the Phase 0 output carefully, as a wrong call there propagates.

## Goal

Separate this codebase into two layers so the connectivity stack can be reused across
future products (robot-vacuum patch, e-meter reader, heat-pump thermostat, …):

1. **`iot_foundation/`** — the reusable **connectivity core**: MQTT client + app,
   Wi-Fi / SoftAP provisioning, OTA/FWU, SNTP, NetBIOS, the base web-UI shell
   (Status / Settings / Wi-Fi / OTA tabs), config/settings store, buildinfo, and the
   per-MCU platform ports. Owns `main()`.
2. **Products** — the hardware/application layer. The existing HVS882 / DRV / relay /
   DIN logic plus the **I/O Config** and **Control** web tabs become the **home-auto**
   product (`products/home_auto/`), the first consumer of the foundation.

Both current MCUs (**TM4C1294**, Cortex-M4; **CC35x1**, Cortex-M33) must keep building
green and be **verified on real hardware** after the restructure.

## Locked decisions (from 20-question intake, 2026-09-14)

| # | Topic | Decision |
|---|-------|----------|
| 1 | Repo split | **Monorepo now, split to separate repos later** once the API stabilizes. |
| 2 | Foundation scope | **Connectivity core only** (MQTT, Wi-Fi/SoftAP, OTA, web shell, config store, SNTP/NetBIOS, PAL, platform ports). |
| 3 | I/O plumbing | Foundation ships a **thin, OPTIONAL** generic channel→MQTT-topic helper; products may use or ignore it. |
| 4 | Web UI | **Static base + product tabs.** Foundation owns Status/Settings/Wi-Fi/OTA HTML; product adds its own pages, merged into one fsdata image at build. |
| 5 | MQTT contract | **Foundation defines base** (device-id, base topic prefix, availability/LWT, OTA/command channels); **product extends** with its own subtopics. |
| 6 | CCS build wiring | **Shared source dir, linked in** (as today's `mqtt_io_common/`) — products reference foundation sources via linked resources / include paths. |
| 7 | MCU porting | **Per-MCU `platform/<mcu>/` dirs** (current pragmatic style), not a formal PAL rewrite. |
| 8 | First product | **Home-auto is the reference product** — carve the existing HW logic + I/O Config & Control tabs onto the extracted foundation. |
| 9 | Verification bar | **Both build + both HW-verified** before done. |
| 10 | Dir naming | **Rename to a neutral name** (foundation is more than "mqtt_io" now). |
| 11 | Foundation name | **`iot_foundation`**. |
| 12 | Product identity | **Compile-time `product_config.h`** (device name, MQTT base topic, firmware id, feature flags) included by the foundation. |
| 13 | App entry | **Foundation owns `main()`** + super-loop/RTOS task; calls fixed product hooks: `product_init()`, `product_poll()`, `product_on_mqtt(topic,msg)`, `product_http(...)`. |
| 14 | OTA | **One `ota.h` foundation interface, per-MCU impl** (TM4C flash-swap vs CC35x1 PSA FWU). |
| 15 | Config store | **Foundation owns the store with namespaced keys**; foundation keys (wifi/mqtt/name) + a reserved product blob region the product manages. One storage backend per MCU. |
| 16 | Tooling layout | **Per-platform tooling stays as-is** (`platform/<mcu>/tools/`); tooling refactor deferred. |
| 17 | Project count | **Foundation demo project per MCU + product projects.** Foundation stays independently buildable. |
| 18 | Migration method | **Branch + worktree spike first** to validate CCS wiring, then redo cleanly on `main`, build-green each step. |
| 19 | Future-proofing | Prove with home-auto **plus a minimal stub second product** (e.g. `emeter` that publishes one dummy value) to keep the foundation API honestly reusable — but no speculative plugin framework (YAGNI). |
| 20 | Deliverable | **This plan doc + memory update** now; code execution is a later, separate effort. |

## Current layout (starting point)

```
mqtt_io_common/            <- today's shared dir (to become iot_foundation/, minus product code)
  buildinfo.{c,h} config.{c,h} mqtt_client.{c,h} ota.h
  common/  mqtt_app  sntp_client  netbiosns  webui  cgifuncs      (connectivity — FOUNDATION)
  common/  io_scan  output_ctrl  relay_pulse  input_events        (home-auto — PRODUCT)
  din_chain.{c,h}  relay_chain.{c,h}                              (HVS882/DRV — PRODUCT)
  fs/  index/wifi_*/cfgrestore/factoryreset/fwupdate/tools/temp   (FOUNDATION pages)
  fs/  iocfg.shtml  control.shtml  iostate.shtml                  (PRODUCT pages)
  pal/                                                            (FOUNDATION)
mqtt_io_tm4c1294/          <- TM4C project (main, board glue io.c/enet_io.c, ota.c, driverlib…)
mqtt_io_cc35x1/            <- CC35x1 project (projectspec-bootstrapped)
platform/cc35x1/           <- CC35x1 SDK glue + tools (flash/preflight/prebuild_fs)
```

The core untangle: `mqtt_io_common/common/` currently **mixes** connectivity (keep) with
home-auto app logic (move to product).

## Target layout

```
iot_foundation/                     # reusable connectivity core; owns main()
  core/     mqtt_client  mqtt_app  sntp_client  netbiosns  buildinfo
  web/      webui shell + base fs pages (index, settings, wifi_*, ota/fwupdate, tools)
  config/   config store (namespaced keys; reserved product blob)
  ota/      ota.h (interface only)
  io/       optional channel->topic helper (thin; product opt-in)
  pal/      pal_gpio/irq/log/storage/str/sys + lwip_compat
  product_api.h                     # product_config + hook contract the foundation calls
  platform/
    tm4c1294/   ota.c, storage/net glue, lwipopts, board bring-up shims
    cc35x1/     PSA-FWU ota, SimpleLink glue, wifi_store, dhcpserver, net_wifi, tools/
products/
  home_auto/                        # product #1 = existing HW app
    app/      io_scan  output_ctrl  relay_pulse  input_events  din_chain  relay_chain
    web/      iocfg.shtml  control.shtml  iostate.shtml
    product_config.h                # device name, base topic, feature flags
    hooks.c                         # product_init/poll/on_mqtt/http implementations
  emeter/                           # product #2 = minimal stub (publishes one dummy value)
    product_config.h  hooks.c  (+ tiny web tab, optional)
docs/…  tools/ (host launchers)
```

CCS projects after restructure (decision 17):
- `foundation_demo_tm4c1294`, `foundation_demo_cc35x1` — buildable base, no product I/O.
- `homeauto_tm4c1294`, `homeauto_cc35x1` — product #1 on both MCUs.
- `emeter_tm4c1294` (or whichever single MCU) — stub product to keep the API honest.
- Each links `iot_foundation/` sources via linked resources + include paths (decision 6).

## Foundation ↔ product contract

`iot_foundation/product_api.h` (the only surface a product must satisfy):

```c
/* provided BY the product, called BY the foundation */
void        product_init(void);                              /* register tabs/topics/channels */
void        product_poll(void);                              /* main-loop tick (CC35x1: under LOCK_TCPIP_CORE) */
void        product_on_mqtt(const char *topic, const uint8_t *msg, uint16_t len);
int         product_http(struct http_state *hs);             /* product CGI/SSI hooks */
/* provided BY the product as data */
#include "product_config.h"  /* PRODUCT_NAME, MQTT_BASE_TOPIC, FW_ID, feature flags */
```

- **main() lives in the foundation** (decision 13); it brings up net → provisioning →
  MQTT → OTA → web, then drives the super-loop calling the product hooks. Products never
  own `main()`.
- **MQTT**: foundation publishes availability/LWT + OTA/command under `MQTT_BASE_TOPIC`;
  product subtopics hang off the same prefix (decision 5).
- **CC35x1 core-lock rule** ([[cc35x1-corelock-publish]]) applies to `product_poll()` —
  any tick that may publish runs under `LOCK_TCPIP_CORE`. Documented in `product_api.h`.
- **Web**: foundation fsdata (base tabs) + product fsdata (iocfg/control) merged into one
  image at build (decision 4); reuse the existing `prebuild_fs` regen path.

## Phases (branch + worktree spike first — decision 18)

**Phase 0 — Spike (throwaway worktree).** Prove the CCS wiring before touching `main`:
rename `mqtt_io_common → iot_foundation`, split `common/` into foundation vs product,
stand up one `foundation_demo_*` project, confirm it links and builds on **one** MCU.
Capture the exact per-file disposition and CCS linked-resource setup. Discard the worktree.

**Phase 1 — Rename + carve foundation (on `main`).** `git mv mqtt_io_common → iot_foundation`;
move the home-auto files out to `products/home_auto/`; add `product_api.h` + a temporary
no-op product so the foundation compiles standalone. **Both MCUs build green.** Commit.

**Phase 2 — Foundation demo projects.** Create `foundation_demo_tm4c1294` +
`foundation_demo_cc35x1` (connect + provision + OTA + base web, no product I/O).
Build green both. Commit.

**Phase 3 — Home-auto as product #1.** Wire `products/home_auto/` (app + iocfg/control
tabs + hooks) into `homeauto_tm4c1294` / `homeauto_cc35x1` via `product_api.h`. Restore full
behavior. **Both build green.** Commit.

**Phase 4 — HW verify (decision 9).** On real TM4C **and** CC35x1: relays toggle over MQTT,
inputs read, I/O Config + Control tabs work, SoftAP provisioning, OTA. CC35x1 verify per repo
rules (COM14 via pyserial, USB power-cycle — [[serial-use-pyserial]], [[cc35x1-provisioning]]).
Commit.

**Phase 5 — Stub second product (decision 19).** Add `products/emeter/` (one dummy MQTT
value, minimal/absent web tab) + its CCS project on one MCU; build green. This is the test
that the foundation API isn't accidentally home-auto-shaped. Commit.

**Phase 6 — Docs.** Write `docs/HOW_TO_ADD_A_PRODUCT.md` and `docs/HOW_TO_ADD_AN_MCU.md`
from the real extraction (a new MCU = a new `iot_foundation/platform/<mcu>/` port +
`ota.h`/config-store/PAL impls). Update `README.md`, close this plan.

## Phase 0 spike findings (2026-09-14, in-place throwaway branch)

Ran a spike on a disposable branch (`spike/plan11-phase0`, since deleted; no source changed).
The build-verify of the rename was intentionally bounded and hit a hard operational wall; the
static coupling analysis fully answered Phase 0's design questions. Results:

1. **The in-place `git mv mqtt_io_common → iot_foundation` is BLOCKED while CCS is open** —
   it fails with *Permission denied* because CCS holds the shared files locked as linked
   resources in both live projects. Closing CCS to release the locks also kills the CCS MCP
   build servers, so a **build-verified rename is impossible in a live-CCS session.**
   → **Phase 1 sequencing (revised):** the opening `git mv` + path find/replace must be done
   with **CCS fully closed**, then reopen CCS and build. Do not expect the CCS MCP to drive the
   rename step. (This also means the "worktree vs in-place" choice is moot for the rename itself —
   neither builds under live CCS.)

2. **The foundation/product split is a REFACTOR, not a file move.** The boundary cuts *through*
   three files, which must be cleaved onto the `product_api.h` hooks before the foundation can
   compile standalone (this is the bulk of Phases 1+3, and the Opus/high judgment work):
   - `common/webui.c` — 1840 lines, ~40 product refs; **hosts the `/iocfg.cgi` + control CGI
     handler table** (the product web logic lives here, not in `cgifuncs.c`).
   - `common/mqtt_app.c` — 847 lines, ~29 product refs; `#include`s all six product headers,
     publishes I/O.
   - `config.c` — 1104 lines, ~16 product refs; product keys mixed into the store (matches
     decision 15's "reserved product blob" — the store needs namespacing).
   Clean foundation (move as-is): `mqtt_client.c` (0 refs), `common/cgifuncs.c` (0 refs).
   Clean product (move as-is): `din_chain`, `relay_chain`, `common/{io_scan, output_ctrl,
   relay_pulse, input_events}`.

3. **Correction to the per-file table below:** `cgifuncs.c` is clean foundation (0 product refs)
   — the "split cgifuncs" plan was misdirected. The CGI that must split is **`webui.c`'s handler
   table**, not `cgifuncs.c`.

4. **Rename blast radius is small/mechanical** (~8 tracked files): `mqtt_io_tm4c1294/.project`
   (14 linked-resource locations) + `.cproject` (3 include paths); `platform/cc35x1/mqtt_io_cc35x1.projectspec`
   (3 includes + ~15 file entries, mixed `action="link"`/`"copy"`); `platform/cc35x1/tools/prebuild_fs.bat`;
   `.claude/settings.json`; `README.md`; `docs/PORTABILITY.md`. Trivial vs. the code untangle.

5. **Open item — locate `main()`.** No `int main` was found by grep in `mqtt_io_tm4c1294/` or
   `platform/cc35x1/` — confirm where the entry point actually lives (likely `enet_io.c` / SDK
   `main_freertos.c`) before wiring decision 13 ("foundation owns `main()`").

**Recommended reorder:** design the `product_api.h` surface + the webui/mqtt_app/config cleave
*first* (the real risk), and treat the rename as a cheap closed-CCS mechanical step done second.

> **The `product_api.h` contract + the three-file cleave (webui/mqtt_app/config) are designed in
> [`docs/FOUNDATION_PRODUCT_API_DESIGN.md`](../FOUNDATION_PRODUCT_API_DESIGN.md)** — read it before
> Phase 1. It also revises the phase ordering: do the in-place cleave under live CCS *first*, then
> the CCS-closed rename.

## Per-file disposition (finalized in Phase 0; initial classification)

| File(s) | Destination |
|---|---|
| `mqtt_client`, `common/sntp_client`, `common/netbiosns`, `buildinfo` | `iot_foundation/core/` |
| `common/mqtt_app` | **SPLIT — mostly PRODUCT.** Thin connect-edge/LWT/subscribe glue → foundation; HA discovery + relay/cover/input/temp publish + command dispatch → `products/home_auto/app/ha_mqtt.c` (via `product_on_connect()`/`product_on_mqtt()`). See design doc. |
| `common/webui` (shell), base `fs/*` (index, wifi_*, cfgrestore, factoryreset, fwupdate, tools, temp, otaack) | `iot_foundation/web/` |
| `config.{c,h}` | `iot_foundation/config/` |
| `ota.h` | `iot_foundation/ota/`; `mqtt_io_tm4c1294/ota.c` → `iot_foundation/platform/tm4c1294/` |
| `pal/*` | `iot_foundation/pal/` |
| `common/io_scan`, `output_ctrl`, `relay_pulse`, `input_events`, `din_chain`, `relay_chain` | `products/home_auto/app/` |
| `fs/iocfg.shtml`, `control.shtml`, `iostate.shtml` | `products/home_auto/web/` |
| `common/cgifuncs` | **foundation as-is** (Phase 0: 0 product refs — clean). The I/O-config/control CGI to split lives in `common/webui.c`'s handler table, not here. |
| `common/webui.c` handler table | **split**: base tabs (Status/Settings/Wi-Fi/OTA) → foundation; `/iocfg.cgi` + control handlers → product via `product_http()` |
| `io.{c,h}`, `enet_io.c`, board glue in `mqtt_io_tm4c1294/` | split board bring-up (foundation platform) vs I/O wiring (product) in Phase 0 |

## Constraints / gotchas (carry into execution)

- Never hand-edit `.project`/`.cproject`/`.syscfg` — use the CCS project/SysConfig MCP.
- `renameProject` **moves the directory** ([[rename-tm4c-project-todo]]); projectspec reimport
  **drops manual build steps** — re-add flash/prebuild steps after any reimport.
- CC35x1 two-copy gotcha ([[cc35x1-web-fs-regen]]): files duplicated in `platform/cc35x1/`
  and the project dir must be re-copied after editing the canonical one.
- Web edits reach firmware only via the fsdata regen path — not by editing HTML alone.
- Build only via the ccs-project MCP `buildProject`, never raw gmake.
- Leave pre-existing uncommitted `platform/cc35x1/{main,net_wifi}` edits untouched.
- See **Reconciliation with Plan 6** below for how this plan absorbs Plan 6's deferred remainder.

## Reconciliation with Plan 6 (deferred "perfect symmetry" remainder)

Plan 6 ([[rename-tm4c-project-todo]], archived DONE 2026-09-14) shipped the rename +
`mqtt_io_common/` extraction but **deferred** three Option-B items — CC35x1 fold, OTA-binary
unify, tools consolidation — gated on "a 3rd platform is real." Plan 6 organized on a **per-MCU
symmetry** axis (make each platform a mechanical clone). Plan 11 organizes on a **foundation/product**
axis. They touch the same files, so resolve the overlap explicitly:

| Plan 6 deferred item | Reconciliation in Plan 11 |
|---|---|
| **Trigger:** "reopen when a 3rd platform lands" | **Retired.** Plan 11 does the restructure now, driven by product reuse (home-auto + `emeter` stub) and readiness for new MCUs — not by a 3rd platform. Plan 6's "add platform N with zero edits to shared" litmus survives as a **validation check**, not a gate. |
| **CC35x1 fold:** move `platform/cc35x1/` → *into the `mqtt_io_cc35x1/` project dir* | **Superseded — different destination.** Plan 11 moves the tracked CC35x1 port to `iot_foundation/platform/cc35x1/` (the port is *foundation*, not product). This resolves Plan 6's blocker (the project dir is projectspec-generated + gitignored, so folding tracked sources into it would commit SDK/generated files): under Plan 11 tracked port sources live in the foundation, the generated project dir stays gitignored. `mqtt_io/platform/tm4c1294/` → `iot_foundation/platform/tm4c1294/` symmetrically. |
| **Tools consolidation** (`platform/cc35x1/tools/` relocate + parameterize) | **Partially:** the `tools/` dir **moves with its platform** into `iot_foundation/platform/cc35x1/tools/`; the *parameterization/dedup* refactor stays **deferred** (Plan 11 decision 16 keeps per-platform tooling as-is). Combine with Plan 7 when done. Repo-root `tools/` still holds host launchers. |
| **OTA-binary unify** (shared `ota/`, `mqtt_io_<platform>_<ts>.bin`) | **Still deferred / out of scope.** Plan 11 keeps the `ota.h` interface + per-MCU impl (decision 14); the shared OTA-archive/naming cleanup is cosmetic and untouched. |

Net: Plan 11 **absorbs and replaces** Plan 6's dormant wishlist. Do **not** reopen archived Plan 6;
the per-MCU-symmetry framing is retired in favor of foundation/product. The `rename-tm4c-project-todo`
memory is updated to point here.

## Out of scope (revisit later)

- Splitting `iot_foundation` into its own git repo (decision 1 — after API stabilizes).
- A runtime tab-registry / plugin framework (decision 4/19 — static tabs + hooks only).
- Tooling refactor to shared parameterized scripts (decision 16 — per-platform stays).
- Building the vacuum / thermostat products (only home-auto + emeter stub now).
