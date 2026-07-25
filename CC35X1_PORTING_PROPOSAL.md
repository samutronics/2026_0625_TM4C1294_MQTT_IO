# CC35x1 Port — Architecture & Development Strategy Proposal

**Status: PROPOSAL — awaiting review/approval. No implementation started.**
Baseline: TM4C1294XL MQTT I/O gateway, released `v1.0.0`.
Target: LP-EM-CC35X1 (CC3551E), same external field-I/O hardware, Wi-Fi + BLE connectivity.

---

## 0. The one finding that reframes everything

The CC3551E is **not** a network co-processor — it is a **single-chip wireless MCU**: a
160 MHz Arm Cortex-M33 (FPU, TrustZone, AI accel) that runs the user application directly. Its
**SimpleLink Wi-Fi SDK ships FreeRTOS + TI Drivers + a BLE 5.4 host + a Wi-Fi 6 supplicant + an
lwIP-based TCP/IP stack**, plus secure OTA with anti-rollback.

That last point is the lever: **the target's networking stack is lwIP — the same stack this
project already uses.** So the port is *not* "rewrite the networking." It is:

1. Move the app to an **RTOS threading model** (FreeRTOS) — non-negotiable, because the Wi-Fi
   supplicant and BLE host require it.
2. Replace **five hardware touch-points** (EEPROM, flash/OTA, GPIO bit-bang, MAC/PHY netif,
   clock/SysTick bring-up) behind a thin Platform Abstraction Layer.
3. Add **Wi-Fi-specific** concerns Ethernet never had: credential provisioning, association
   state machine, and (optionally) BLE for commissioning/local control.

Roughly **~65% of the ~10,500 lines of first-party logic is portable with only a logging/snprintf
shim** — the MQTT topic scheme + Home Assistant discovery, the shutter/timed output FSM, input
debounce/click detection, config record layout + CRC, all CGI/SSI handler bodies, and the **entire
`fs/` web UI** carry over. The concentrated work is the HAL and the RTOS transition.

Sources: [CC3551E](https://www.ti.com/product/CC3551E) · [LP-EM-CC35X1](https://www.ti.com/tool/LP-EM-CC35X1) · [LP-EM-CC35X1 User's Guide (SWRU629)](https://www.ti.com/lit/ug/swru629a/swru629a.pdf)

---

## 1. Assessment of the existing TM4C1294XL architecture

Bare-metal, `NO_SYS=1` lwIP 1.4.1 (raw/callback API), TI Arm Clang, CCS 2100. A `main()` super-loop
ticks the application at 10 ms (SysTick); **all TCP/IP + HTTP + MQTT-RX work runs in Ethernet-ISR
context.** ~10,540 lines first-party across ~30 modules + a ~566 KB generated web filesystem.

Layered (as-built, boundaries are real):

| Layer | Modules |
|-------|---------|
| Application / domain | `output_ctrl` (shutter/timed FSM), `input_events` (debounce/click), `relay_pulse`, `io` (legacy demo — droppable) |
| Networking / middleware | `mqtt_app` (topics + HA discovery), `mqtt_client` (raw-TCP MQTT 3.1.1), `netbiosns`, `sntp_client` |
| Device drivers | `din_chain` (SN65HVS882), `relay_chain` (DRV8860), `ota` (flash apply) |
| Config / persistence | `config` (magic+CRC32 records in on-chip EEPROM) |
| HAL / board / startup | `board_pins.h`, `startup_ccs.c`, `lwipopts.h`, `enet_io.c` `main()` |
| Web UI glue | `enet_io.c` (CGI/SSI handlers), `io_fs.c`, `cgifuncs.c`, `fs/*.shtml` → `io_fsdata.h` |

## 2. Strengths & weaknesses of the current design

**Strengths**
- **Domain logic is already well isolated.** `mqtt_app`, `output_ctrl`, `input_events`,
  `relay_pulse`, `cgifuncs` touch essentially no hardware — only `ustdlib`/`uartstdio` shims.
- **Same TCP/IP stack as the target (lwIP).** DHCP/DNS/SNTP/NetBIOS/httpd/MQTT app code is
  stack-portable in principle.
- **Config as versioned records** (magic + CRC32, self-healing defaults, `_Static_assert` layout
  guards) — a clean, storage-agnostic pattern once the raw EEPROM calls are abstracted.
- **Web UI is pure data** (`fs/` → `io_fsdata.h`): fully portable, gives identical UX for free.
- **OTA design** (staged region, CRC, pending-flag handshake) maps conceptually to SimpleLink OTA.

**Weaknesses (the things the port must fix)**
- **`enet_io.c` is a 2,659-line god-file** mixing `main()`, clock/EMAC/SysTick bring-up, factory
  reset, *and* every CGI/SSI handler body. Must be split (HAL out, handlers into a `web_handlers`
  module) before it can be shared.
- **NO_SYS + networking-in-ISR won't survive the move to an RTOS.** Under FreeRTOS, lwIP runs in a
  tcpip thread; ISRs must hand off to tasks. This is the single biggest structural change.
- **Hardware coupling is scattered**, not layered: direct `EEPROMProgram`, `FlashProgram`,
  `GPIOPin*`, `SysCtlClock*` calls sit inside otherwise-portable files (`config.c`, `ota.c`,
  `din/relay_chain.c`). No HAL seam exists yet.
- **Global mutable state assumes a single thread.** Safe today (cooperative super-loop); becomes a
  concurrency hazard under FreeRTOS without explicit ownership/locking.
- **No unit tests, no CI.** The fragile two-makefile `ORDERED_OBJS` object-list maintenance has
  already bitten this project (a missing `.o` broke the link). A second target multiplies that risk.
- **Bit-banged chain drivers** — fine functionally, but re-timing them on a 160 MHz M33 under an
  RTOS scheduler needs care (better: use the CC35x1's hardware SPI).

## 3. Recommended architecture for CC35x1

**FreeRTOS-based, lwIP raw-API kept where possible, structured around a Platform Abstraction Layer.**
Threading model:

- **`tcpip` thread** (lwIP) — owns the stack; httpd, mqtt_client, netbios, sntp run here via raw
  API (same code as today), executed in the tcpip thread context rather than an ISR.
- **`app` task** — the 10 ms tick: `output_ctrl`, `input_events`, chain scan, config commits.
- **`wifi`/`conn` task** — association + provisioning state machine (new; replaces "Ethernet link
  transition" trigger).
- **`ble` task** (optional, CC35x1-only) — commissioning + local control.
- SDK-owned tasks — Wi-Fi supplicant, BLE host — provided by the SimpleLink SDK.

App/domain code stays single-writer per module; cross-task interaction goes through lwIP's
thread-safe primitives and a small number of queues/mutexes, so the domain logic keeps its current
straight-line style.

## 4. Bare-metal vs RTOS — recommendation & justification

**Recommendation: FreeRTOS** (TI-supported default for the SimpleLink Wi-Fi SDK).

| Option | Verdict |
|--------|---------|
| **Bare-metal** | **Rejected.** The Wi-Fi supplicant and BLE 5.4 host in the SDK are built on an RTOS; there is no supported NO_SYS path for CC35x1 connectivity. Bare-metal would mean rewriting the vendor stacks — untenable. |
| **FreeRTOS** | **Recommended.** It's what the SimpleLink Wi-Fi SDK ships and validates on this silicon: best driver/stack support, examples, and long-term TI backing. Small, well-understood, easy to debug. lwIP integrates cleanly (tcpip thread). |
| **TI-RTOS / SYS/BIOS** | **Rejected.** Legacy; TI moved SimpleLink to FreeRTOS + TI Drivers. Not offered for new parts. |
| **Zephyr** | **Viable, deferred.** TI lists Zephyr support for CC35x1, and it's the strongest long-term/portability story (own lwIP-equivalent net stack, device tree, upstream OTA). But: steeper learning curve, a brand-new BSP on brand-new silicon, and it would diverge the two platforms' OS abstraction more than FreeRTOS. Revisit only if the product line grows toward Linux-class or multi-vendor targets. |

Justification weighting *robustness, reliability, debuggability, long-term support* highest for a
field device that is **OTA-recovery-only**: FreeRTOS is the lowest-risk path that the vendor
actively supports on this chip. We isolate the choice behind an OS-abstraction (§7) so a later
Zephyr move is possible without touching domain logic.

## 5. Repository strategy

**Recommendation: one monorepo, restructured into `common/` + `platform/<name>/`, migrated
incrementally.** (Option 1, executed cleanly — not "two tangled projects in one tree.")

| Option | Assessment |
|--------|-----------|
| **1. Monorepo, shared core + platform dirs** | **Recommended.** One place to change shared behavior → both platforms stay in lockstep by construction. Atomic cross-platform commits. Simplest for a solo maintainer who commits to `main`. Matches how you already work. |
| 2. New standalone repo, copy as foundation | Fast to start, but **guarantees drift** — every shared fix must be double-applied by hand. Worst long-term sync story. Rejected. |
| 3. Shared `common` repo + per-platform repos via submodule/subtree | Cleanest *theoretical* separation, but submodules add real friction (detached HEADs, two-step commits, CI complexity) that isn't justified for two targets and one developer. Keep as a **future escape hatch** if a third-party or separate team ever owns one platform. |

**What goes where**
- `common/` — everything portable: `mqtt_app`, `mqtt_client`, `output_ctrl`, `input_events`,
  `relay_pulse`, `sntp_client`, `netbiosns`, `cgifuncs`, the `config` record/CRC/accessor layer,
  the chain **protocol** logic (SN65HVS882/DRV8860 framing), the web CGI/SSI **handler bodies**,
  and all of `fs/`.
- `platform/tm4c1294/` — `startup_ccs.c`, `board_pins.h`, `enet_io_ccs.cmd`, EMAC netif
  (`utils/lwiplib`), EEPROM/flash PAL impls, TM4C clock/SysTick, the CCS project files.
- `platform/cc35x1/` — FreeRTOS config, SysConfig, Wi-Fi netif + provisioning, BLE, SFLASH storage
  PAL, SimpleLink OTA glue, linker cmd, its CCS project.
- `pal/` — the abstraction **interfaces** (headers) both platforms implement.
- `docs/`, `tools/` (makefsfile step, host-test harness), `tests/`.

**Keeping them synced:** they share source by *inclusion*, not copy — a change in `common/` compiles
into both. CI (see §12) builds **both** targets on every push so a shared-code change that breaks
one platform fails immediately.

## 6. Shared architecture proposal (layering)

```
        ┌─────────────────────────────────────────────┐
        │        Application / Domain (common/)        │  output_ctrl, input_events,
        │  platform-independent behavior & user model  │  relay_pulse, config records
        ├─────────────────────────────────────────────┤
        │        Services (common/)                    │  mqtt_app + mqtt_client,
        │  web handlers, MQTT, discovery, SNTP, naming │  cgifuncs, netbios, httpd glue, fs/
        ├───────────────┬─────────────────────────────┤
        │  net_link      │   PAL: storage, gpio/spi,   │  <-- the only seam that differs
        │  (assoc/DHCP)  │   ota, time, log, sys        │
        ├───────────────┴─────────────────────────────┤
        │   OS abstraction (osal): task/mutex/queue    │  thin: FreeRTOS on CC35x1,
        │                                              │  super-loop shim on TM4C
        ├─────────────────────────────────────────────┤
        │   Platform (platform/<name>/) + vendor SDK   │  TivaWare | SimpleLink+FreeRTOS
        └─────────────────────────────────────────────┘
```

Rule: **dependencies point downward only.** `common/` may include `pal/*.h` and `osal/*.h`, never a
vendor header. That single rule is what keeps the two platforms aligned.

## 7. Platform abstraction strategy (the PAL/OSAL seam)

Small, concrete interfaces — abstract exactly the five touch-points, nothing more (avoid a
speculative "HAL for everything"):

| Interface | Replaces today's… | TM4C impl | CC35x1 impl |
|-----------|-------------------|-----------|-------------|
| `pal_storage` (keyed read/write blob) | direct `EEPROMRead/Program` in `config.c` | on-chip EEPROM | SFLASH file/emulated-EEPROM |
| `pal_gpio` / `pal_spi` | `GPIOPin*` bit-bang in `din/relay_chain.c` | TivaWare GPIO/SSI | SimpleLink GPIO/SPI (HW SPI) |
| `pal_ota` (stage/verify/apply) | `FlashProgram` + SRAM apply in `ota.c` | TM4C flash | SimpleLink secure OTA |
| `pal_time` (monotonic ms, tick) | SysTick | SysTick | FreeRTOS tick / SysTick |
| `pal_log` (`printf`) | `UARTprintf` | UART0 ICDI | UART / RTT |
| `pal_sys` (clock init, reset, crit-section) | `SysCtlClock*`, `NVIC` | TivaWare | SimpleLink |
| `osal` (task/mutex/queue/delay) | — (super-loop today) | thin cooperative shim | FreeRTOS |
| `net_link` (bring-up, is-connected, on-change) | lwIP link-detect | Ethernet link | Wi-Fi assoc + provisioning |

`config.c` splits into `config_records.c` (common: layout, CRC, accessors, self-heal) + a
`pal_storage` backend. `din/relay_chain` keep their shift-register framing in `common/` and call
`pal_gpio`/`pal_spi`.

## 8. Hardware abstraction strategy

The **external field-I/O boards are identical** across platforms — so the SN65HVS882 / DRV8860
*protocol* is shared domain code; only the pin-level transport (`pal_gpio`/`pal_spi`) differs.
Recommendation: on CC35x1 drive the chains over **hardware SPI** (the M33 has proper SPI + DMA)
rather than porting the bit-bang — cleaner and RTOS-scheduler-safe. Pin maps live in
`platform/<name>/board_pins.h` (TM4C) and SysConfig (CC35x1). The legacy `io.c` PWM/LED demo is
dropped.

## 9. Networking abstraction strategy

**Common (both platforms):** lwIP + httpd + `mqtt_client` + `mqtt_app` (topics/HA discovery) +
`netbiosns` + `sntp_client` + DHCP/DNS. The application talks only to these; it never knows the
link type.

**Platform-specific, behind `net_link`:**
- **TM4C:** Ethernet — DHCP starts on physical link transition (today's behavior).
- **CC35x1:** Wi-Fi station — association state machine, reconnect/backoff, and **credential
  provisioning** (new). Provisioning via (a) a web page in SoftAP mode on first boot, and/or
  (b) **BLE commissioning** (see below). DHCP runs once associated.

**BLE (CC35x1-only, kept fully optional):** exposed as a `local_ctrl`/`provisioning` service
interface that only `platform/cc35x1` implements. TM4C never compiles it, so BLE cannot affect the
Ethernet platform. Two justified BLE uses: (1) headless Wi-Fi commissioning; (2) local
control/status when Wi-Fi/broker is down. Keep the MQTT model as the source of truth; BLE is a
transport onto the same `output_ctrl` commands.

**Naming/discovery:** keep NetBIOS; consider adding **mDNS** on CC35x1 (SimpleLink supports it) for
`http://<clientid>.local/` on Apple/Android, closing the gap NetBIOS leaves.

**Confirm in Phase 0 (top risk):** whether the SDK's lwIP permits **raw-API** use from the tcpip
thread. If it forces the **socket/netconn** API, the *transport binding* of `mqtt_client` and
`httpd` is re-implemented against sockets (a few hundred lines) — but `mqtt_app` topics/discovery,
`output_ctrl`, `config`, `cgifuncs`, and `fs/` are unaffected. Domain logic is safe either way.

## 10. Recommended repository / directory structure

```
repo-root/
├─ common/
│  ├─ app/        output_ctrl, input_events, relay_pulse
│  ├─ services/   mqtt_app, mqtt_client, sntp_client, netbiosns, web_handlers (CGI/SSI bodies)
│  ├─ config/     config_records.c/h (layout+CRC+accessors), cgifuncs
│  ├─ drivers/    din_chain, relay_chain  (protocol only; call pal_gpio/pal_spi)
│  └─ web/        fs/*  +  generated io_fsdata.h
├─ pal/           *.h interfaces: pal_storage, pal_gpio, pal_spi, pal_ota, pal_time, pal_log,
│                 pal_sys, net_link, osal
├─ platform/
│  ├─ tm4c1294/   startup, board_pins.h, EMAC netif, EEPROM/flash PAL, super-loop osal, CCS proj, .cmd
│  └─ cc35x1/     FreeRTOS cfg, SysConfig, Wi-Fi netif+provisioning, BLE, SFLASH PAL, SimpleLink OTA, CCS proj, .cmd
├─ third_party/   lwIP bits we vendor (httpd.c), shared
├─ tools/         makefsfile web-FS build step, host-test scaffolding
├─ tests/         host unit tests for common/ (Unity/Ceedling or plain)
├─ docs/          README, HARDWARE, this proposal, per-platform notes
└─ .github/workflows/  CI: build both targets + run host tests
```

## 11. Build-system strategy

Both platforms use the **same TI Arm Clang compiler** and CCS/gmake — so the diagram's "common
source → two binaries" is practical, with two caveats: CC35x1 adds **SysConfig** codegen and links
the **SimpleLink SDK + FreeRTOS**; TM4C links **TivaWare** and has no SysConfig. So: **shared source
tree, two CCS projects, one shared pre-build step.**

- Each `platform/<name>/` has its own CCS project + linker `.cmd` + compiler/defines, and pulls in
  `common/`, `pal/`, `third_party/` via include paths + source references.
- The **web-FS regen** (`makefsfile fs → io_fsdata.h`) becomes one shared script in `tools/`, run
  as a pre-build step by both (kills the "regenerate + `rm Debug/io_fs.o`" foot-gun).
- **Kill the `ORDERED_OBJS` foot-gun:** generate object lists from a source glob (or a per-dir
  `.mk` include) so adding a `common/` file never silently breaks a link. This has already burned
  this project once and would burn it twice with two targets.
- A top-level `make tm4c` / `make cc35x1` / `make all` wrapper (or a small script) drives both for
  local dev and CI. Post-build keeps emitting the timestamped `.bin` per platform.

## 12. CI/CD strategy

Currently: none (local gmake). Recommend **GitHub Actions**:
- **On every push/PR:** build **both** targets headlessly with the TI toolchain + run **host unit
  tests** for `common/`. This is the guardrail that keeps a shared-code change from breaking a
  platform unnoticed — directly addressing the recurring link-list/regression pain.
- **Artifacts:** upload both timestamped `.bin`s per build.
- **On tag:** package a release with both binaries + notes (extend today's `v1.0.0` flow).
- Hardware-in-the-loop fl/flash is out of scope for CI (OTA-only, no bench probe in the loop);
  keep manual OTA verification as the release gate, but a self-hosted runner with a probe is a
  later option.

## 13. Testing strategy

The portable core is pure logic → **testable on a host PC**, no hardware:
- **Unit tests (`tests/`, host build):** shutter/timed FSM (`output_ctrl`), debounce/click
  (`input_events`), config record round-trip + CRC + self-heal (against a fake `pal_storage`), MQTT
  topic + HA-discovery JSON generation (`mqtt_app`), CGI param parsing (`cgifuncs`). Mock the PAL.
- **Integration (on hardware, web + MQTT, as today):** the existing manual matrix — DHCP/assoc,
  web UI + EEPROM/SFLASH persistence across reboot, MQTT lifecycle, relay/input chains, shutter
  interlock, OTA round-trip — run per platform.
- **New for CC35x1:** Wi-Fi reconnect/roaming, provisioning, BLE commissioning, secure-OTA +
  anti-rollback.

Host unit tests are the highest-ROI new investment: they harden the shared core once and protect
**both** platforms.

## 14. Shared vs platform-specific — the line

| Shared (`common/`, ~65% of first-party LOC) | Platform-specific |
|---|---|
| MQTT topics + HA discovery (`mqtt_app`) | Wi-Fi assoc + provisioning; Ethernet link |
| MQTT client protocol (`mqtt_client`)\* | MAC/PHY netif (EMAC vs Wi-Fi) |
| Shutter/timed FSM (`output_ctrl`) | EEPROM vs SFLASH storage backend |
| Input debounce/click (`input_events`) | Flash/OTA apply backend |
| Config records + CRC + accessors | GPIO/SPI transport for the chains |
| Chain protocol (SN65HVS882/DRV8860) | Clock/tick/reset bring-up, startup, linker |
| Web UI (`fs/`) + CGI/SSI handler bodies | BLE (CC35x1-only) |
| SNTP, NetBIOS, cgifuncs | SysConfig (CC35x1-only), CCS project files |

\* transport binding may be re-done if the SDK mandates sockets (§9); the logic stays shared.

## 15. Risks & trade-offs

| # | Risk | Impact | Mitigation |
|---|------|--------|-----------|
| 1 | SDK lwIP forces socket API (no raw) | Re-do httpd + mqtt transport | Confirm in Phase 0; domain logic is insulated either way |
| 2 | RTOS concurrency bugs vs today's single-thread simplicity | Reliability regressions | Explicit task ownership; lwIP tcpip-thread rule; minimal shared state + mutexes; host tests |
| 3 | Brand-new silicon + SDK (2026) maturity | Tooling/stack churn | Pin SDK version; Phase-0 spike before committing; keep TM4C as the proven reference |
| 4 | No on-chip EEPROM on CC35x1 | Persistence rework | `pal_storage` over SFLASH/emulated-EEPROM; record layout unchanged |
| 5 | Wi-Fi reliability + provisioning UX (vs plug-in Ethernet) | Field usability | Robust reconnect/backoff; web SoftAP + BLE commissioning; clear "not provisioned" state |
| 6 | Secure boot / TrustZone / signed OTA complexity | Slower OTA bring-up | Stage it: unsigned dev OTA first, secure-OTA + anti-rollback as a hardening phase |
| 7 | `common/` carve-out destabilizes the *released* TM4C | Regression on OTA-only device | Migrate incrementally with build+OTA verification; re-release TM4C as `v1.0.1` proving parity; keep prior `.bin` |
| 8 | BLE scope creep | Timeline | BLE strictly optional, behind an interface; ship Wi-Fi parity first |

## 16. Migration strategy (protect the released TM4C)

The TM4C is live and **recoverable only via OTA** — so the restructure must never leave `main`'s
TM4C build broken:
1. Carve `common/`/`pal/` by **moving** portable files and pointing the *existing* TM4C project at
   them, **one module at a time**, building + (where behavior could change) OTA-verifying after each
   move. No logic changes during the move — pure relocation + include-path updates.
2. Introduce the PAL seam under the TM4C by implementing the interfaces with today's TivaWare calls
   (behavior-preserving refactor of `config.c`/`ota.c`/chain drivers).
3. **Re-release TM4C `v1.0.1`** from the restructured tree — this *is* the regression proof that the
   shared core is faithful before any CC35x1 code exists.
4. Only then start `platform/cc35x1/` against the now-stable `common/`.

## 17. Phased implementation plan

| Phase | Goal | Exit criterion |
|-------|------|----------------|
| **0. Spike / de-risk** | LP-EM-CC35X1 + SDK bring-up; answer the lwIP raw-vs-socket + SFLASH + OTA questions; Wi-Fi "hello world" serving one static page | Risks 1/3/4/6 resolved on bench; go/no-go on FreeRTOS+raw-lwIP plan |
| **1. Restructure (TM4C only)** | `common/`+`pal/`+`osal` carve; TM4C builds against it; host unit tests for the core; CI builds TM4C + runs tests | TM4C **`v1.0.1`** OTA-verified at feature parity with `v1.0.0` |
| **2. CC35x1 skeleton** | FreeRTOS + lwIP netif over Wi-Fi (hardcoded creds); `pal_storage` (SFLASH); `pal_spi` chain drivers; httpd + `fs/` web UI live | Web UI reachable over Wi-Fi; config persists across reboot; relays switch |
| **3. Functional parity** | `mqtt_app` topics + HA discovery + `output_ctrl` + input chain over Wi-Fi | CC35x1 behaves identically to TM4C from HA/MQTT + web POV |
| **4. Connectivity UX + OTA** | Wi-Fi provisioning (web SoftAP + BLE commissioning), reconnect/backoff, SimpleLink OTA, mDNS/NetBIOS naming | Headless field commissioning + OTA update both work |
| **5. Hardening / release** | Secure boot/signed OTA + anti-rollback, BLE local control (optional), soak test | CC35x1 `v1.1.0`; CI builds both; docs aligned |

## 18. Estimated complexity & technical debt

Rough sizing (single developer):

| Work | Complexity | Notes |
|------|-----------|-------|
| `common/`/`pal` carve + TM4C refactor (Phase 1) | **Medium** | Mechanical but must be careful on a released device; big long-term payoff |
| Host unit tests + CI | **Low–Medium** | High ROI; mostly new, no hardware |
| CC35x1 FreeRTOS + lwIP + web bring-up (Phase 2) | **Medium–High** | New SDK/RTOS; the raw-vs-socket answer swings this |
| MQTT/app parity (Phase 3) | **Low–Medium** | Mostly reused; wiring + concurrency review |
| Wi-Fi provisioning + OTA (Phase 4) | **High** | New domain; provisioning UX + secure OTA are fiddly |
| BLE (Phase 5, optional) | **High** | Entirely new; keep optional |

**Debt to watch:** (a) `enet_io.c` must actually be split, not copied — resist the temptation to
fork it; (b) two OSAL backends (real FreeRTOS vs super-loop shim) must stay honest or bugs hide on
the untested one — mitigated by host tests; (c) SysConfig-generated code on CC35x1 vs hand-written
TM4C pin setup is an unavoidable asymmetry — keep it quarantined in `platform/`.

---

## Recommendation summary — DECISIONS LOCKED (2026-07-25)

1. **RTOS: FreeRTOS** ✅ approved (SDK default; Zephyr remains a documented future option).
2. **Repo: single monorepo** ✅ approved — `common/` + `pal/` + `platform/<name>/`, migrated
   incrementally, *not* a forked second repo.
3. **BLE scope: BLE provisioning FIRST** ✅ revised 2026-07-25 (was "Wi-Fi parity first, BLE later").
   Rationale from Phase 0: the SDK ships BLE (NimBLE GATT) provisioning ready-made and provides *no*
   SoftAP web-provisioning path, so BLE onboarding is the lower-effort route and de-risks the
   toolchain/flash path early. Plan: bring up the SDK `ble_wifi_provisioning` demo first (commission
   Wi-Fi creds from a phone), then proceed to Wi-Fi/MQTT/web parity. BLE stays CC35x1-only behind an
   interface; TM4C never compiles it.
4. **Reuse:** keep lwIP + our httpd/MQTT/web stack shared; abstract only the five hardware
   touch-points + OS + net-link. ~65% of first-party logic and 100% of the web UI carry over.
5. **Protect the release:** restructure TM4C first and re-cut `v1.0.1` as regression proof before
   writing any CC35x1 code.
6. **De-risk first:** a Phase-0 bench spike answers the raw-vs-socket lwIP question (the top
   unknown) before committing the plan.

**Next step:** Phase 0 — bench spike on the LP-EM-CC35X1 (SDK bring-up; confirm raw-vs-socket lwIP,
SFLASH storage, OTA path; Wi-Fi "hello world" serving one static page). Nothing in `common/` or the
TM4C tree is touched until Phase 1.

---

## Appendix A — Phase 0 findings (desktop de-risk, 2026-07-25)

Investigated the installed **`simplelink_wifi_sdk_10_10_01_08`** (CC3551E / LP-EM-CC35X1) +
`FreeRTOSv202212.01`. SDK guidance: **always start from `network_terminal_LP_EM_CC35X1_freertos_ticlang`**
(pre-wired Wi-Fi/BLE/FreeRTOS/lwIP). Results resolve the major risks — most **favorably**:

| Area | Finding | Verdict for our code |
|------|---------|----------------------|
| **lwIP API model** | `NO_SYS=0`, `LWIP_SOCKET=1`, `LWIP_NETCONN=1` — RTOS/sequential (sockets+netconn). BUT the raw lwIP `httpd.c` + `apps/mqtt` also ship and are in the lib build, running on the `tcpip_thread`. | Risk #1 **downgraded**: no forced socket rewrite. |
| **HTTP server** | lwIP `httpd.c` (CGI/SSI/POST/HTTPS) ships + `makefsdata` web-FS tool present (same family as our `makefsfile`). No demo wires it up. | **REUSE + re-integrate.** Web UI, CGI/SSI handler bodies, `fs/` port over; we redo init/task-start. |
| **MQTT** | Demo uses lwIP `apps/mqtt` (`mqtt.h`) + TI `mqtt_if` adapter. LWT, retained, wildcard sub, QoS0/1/2 all supported. | **REWRITE-TRANSPORT (thin).** Keep `mqtt_app` topics/HA-discovery; retarget to `mqtt_client_*`/`MQTT_IF_*`. Our custom `mqtt_client.c` likely retired for the SDK's. |
| **Config storage** | No user-writable internal flash. NVS driver → **external OSPI serial flash** (`XMEMWFF3`); above it `NVOCMP`/`NVINTF` key-value store w/ page compaction. Optional SPIFFS. | **REPLACE backend (`pal_storage`).** Record layout+CRC reused; items stored via `NVS_*`/`NVINTF writeItem`. |
| **OTA** | **PSA Firmware Update** (`psa_fwu_*`): HTTPS download → A/B dual slots → staged → TRIAL→accept/reject rollback; manifest + BL2 secure boot + anti-rollback. | **REPLACE with SDK.** Drop custom chunked OTA; keep only HTTPS-download glue. Far more capable/secure. |
| **Provisioning** | **BLE (NimBLE GATT)** demo delivers SSID/pw → `Wlan_Connect()` → stored in NVS. **No SoftAP web-provisioning** in the SDK. | Tension w/ decision #3: the *ready-made* path is BLE; SoftAP web page is DIY on our httpd. Revisit at Phase 4. |
| **GPIO/SPI** | TI Drivers `GPIO_write/read/toggle`, `SPI_open/transfer` (controller mode, all POL/PHA). Pins routed via SysConfig. | Confirms `pal_gpio`/`pal_spi` + **hardware SPI** for the chains. |

**Net effect on the plan:** reuse is *higher* than estimated — the web UI + CGI/SSI + MQTT topic
logic all carry over. The concentrated new work narrows to: (1) `pal_storage` over NVS/NVOCMP,
(2) OTA re-platformed onto PSA FWU, (3) Wi-Fi bring-up + provisioning UX, (4) the FreeRTOS
threading model. **Risk register update:** #1 downgraded (raw httpd/mqtt available); #4 confirmed
(NVS on external OSPI flash — expected); #6 now a *feature* not a hurdle (PSA secure OTA is provided).

**Empirical items deferred to Phase 0b (needs board on bench):** confirm `LWIP_HTTPD`/CGI/SSI are
enabled in the chosen project's `lwipopts.h`; confirm exact LP-EM-CC35X1 SPI/GPIO pin availability
via SysConfig; build `network_terminal` and serve one static page over Wi-Fi.

**Provisioning re-decision worth surfacing:** since the SDK hands you BLE provisioning ready-made
and gives *no* web-SoftAP path, "BLE later" now costs *more* upfront (build SoftAP yourself) than
adopting BLE onboarding. Not reopening decision #3 now — flagged for Phase 4.
