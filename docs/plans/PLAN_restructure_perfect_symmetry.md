# Plan 6 — Restructure to perfect platform symmetry

**Priority:** Med · **Status:** PARTIAL · Memory: `rename-tm4c-project-todo`
**⚠ Big, high-risk refactor. A smaller model MUST do it incrementally and keep both builds green after every step. If unsure, do Option A only and stop.**

## Progress (2026-09-04)
Executed as **Option B**, Model 2 (sources in tracked project folders), incrementally,
both builds green after each commit:
- **Stage A** (`788a50f`): renamed CCS project `mqtt_io` → `mqtt_io_tm4c1294` (renameProject
  MOVES the dir); post-build (`makefile.targets`/`post_build.ps1`) emits `mqtt_io_tm4c1294_*.bin`.
  CC35x1 projectspec repointed + reimported. Made the CC35x1 projectspec faithful + reimport-lossless:
  encoded `preBuildStep` (FS regen) + `postBuildStep` (createbin sign), and added 4 sources the
  tracked spec had drifted from (buttons/dhcpserver/temp_sensor/wifi_store). `createbin.bat` pause
  guarded (`CCS_BUILD`) for headless builds.
- **Stage C1** (`1d85abb`): created tracked `mqtt_io_common/` holding ALL shared code
  (`common/ fs/ pal/` + `buildinfo config mqtt_client din_chain relay_chain ota.h`). `ota.c` stays
  TM4C-only (CC35x1 stubs OTA); `ota.h` is the shared interface. TM4C `.project`/`.cproject`
  hand-edited (CCS.md rule **waived by user** for this restructure) to link common; CC35x1 projectspec
  repointed to common. `prebuild_fs.bat` FS input → `mqtt_io_common/fs`.

**Key learnings baked in:** `renameProject` moves the on-disk dir; projectspec reimport silently drops
manually-added build steps (fix: encode pre/post steps in the projectspec) and the tracked CC35x1
projectspec had drifted from the live project.

## Deferred (by user decision 2026-09-04)
- **CC35x1 fold** (`platform/cc35x1/` → `mqtt_io_cc35x1/`): NOT done. The CC35x1 project dir is
  projectspec-generated + gitignored and holds SDK-copied + generated files (`main_freertos.c`,
  `httpd.c`, 501 KB `fsdata.c`, …); folding + tracking it would commit regenerable/SDK files to git.
  Chosen layout: keep `platform/cc35x1/` as CC35x1's tracked source dir, project dir gitignored.
  → The two platforms are intentionally asymmetric here (TM4C = hand-owned managed-build project;
  CC35x1 = projectspec-bootstrapped from the SDK). Perfect folder symmetry is not pursued.
- **Stage D (OTA-binary unify)**: SKIPPED — cosmetic, touches the flash path, only affects gitignored
  `Debug/ota/` artifacts. Revisit if a shared OTA archive is wanted.
- **Tools consolidation** (`platform/cc35x1/tools/` → `mqtt_io_cc35x1/tools/`): DEFERRED, do it AS PART OF
  the CC35x1 fold above (not standalone). The platform build/flash scripts (`flash.sh`, `preflight.sh`,
  `makefsdata.py`, `ota_push.py`, `prebuild_fs.bat`) belong with the CC35x1 platform, so when
  `platform/cc35x1/` folds into `mqtt_io_cc35x1/`, its `tools/` moves with it. Keep the repo-root
  `tools/` for host/cross-platform helpers + launchers (`mqtt_*.py`, `createbin.bat`, `flash.bat`,
  `temp/`) — do NOT merge the platform scripts into it. **Cost of moving now (why it waits):** `flash.sh`
  is referenced by `CLAUDE.md`, the CC35x1 **pre-build step** (`prebuild_fs.bat`) + **post-build**
  launchers, `.gitignore` (`platform/cc35x1/tools/*.log`), `settings.local.json`, 7+ plan docs, and
  internal cross-calls (`flash.sh` sources `preflight.sh`; calls `ota_push.py`; `prebuild_fs.bat` calls
  `makefsdata.py`) — plus it forces re-editing the CC35x1 pre/post-build paths in the CCS UI again.
  Update all of those in the same step as the fold. See Option B steps 3 & 5 (which already assume the
  scripts move) and combine with Plan 7 (portability) so paths land parameterized.

## Goal
Make the two platforms perfectly symmetric so a new platform is a mechanical clone:
```
mqtt_io_common/      # ALL shared code + fs/ + OTA binaries (ota/)
mqtt_io_tm4c1294/    # TM4C-only PAL + project files, links into common
mqtt_io_cc35x1/      # CC35x1-only PAL + project files, links into common
```
Acceptance / litmus test: "add platform N" = copy a platform folder, rename the `<platform>` slot, swap the PAL — **zero edits to `mqtt_io_common/`**.

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`; memory `rename-tm4c-project-todo`, `build-tooling`, `cc35x1-web-fs-regen`.

## Current reality (verified)
- CCS TM4C project name = `mqtt_io` (`mqtt_io/.project`). The `mqtt_io/` dir mixes **shared** code (`common/`, `fs/`, `config.*`, `mqtt_client.*`, `io*.c`, `ota.c`…) with **TM4C-only** code (`enet_io.c`, `board_pins.h`, `driverlib/`, `drivers/`, `startup_ccs.c`, `lwipopts.h`, `enet_io_ccs.cmd`) and `mqtt_io/platform/tm4c1294/`.
- `mqtt_io_cc35x1/.project` **links ~12 resources from `mqtt_io/common/`** (and `fs`).
- CC35x1 canonical sources live at the **workspace root** `platform/cc35x1/` (asymmetric with the TM4C's `mqtt_io/platform/tm4c1294/`).

## TWO OPTIONS — pick with the user
### Option A — project-name-only (LOW risk, do this first / maybe only this)
- Rename just the CCS project identity `mqtt_io` → `mqtt_io_tm4c1294` using the **ccs-project MCP `renameProject`** (edits `<name>` in `.project`). Do NOT move any directory.
- CC35x1 links reference the `mqtt_io/` **path**, not the project name, so they keep working.
- Update references to the project *name* (not path): `CLAUDE.md`, `README.md`, docs. Rebuild both projects green.
- Result: symmetric project *names*; directory layout unchanged. STOP here unless the user wants Option B.

### Option B — full restructure (HIGH risk, only when a 3rd platform is real)
Do in small, independently-verifiable steps, building both projects green after each:
1. **Create `mqtt_io_common/`** and `git mv` the shared tree into it (`common/`, `fs/`, `config.*`, `mqtt_client.*`, `io*.c`, `ota.*`, `netbiosns.*`, shared headers). Update BOTH projects' include paths / linked-resource paths to point at `mqtt_io_common/…`.
2. **Create `mqtt_io_tm4c1294/`** (thin): `git mv` TM4C-only files (`enet_io.c`, `board_pins.h`, `driverlib/`, `drivers/`, `startup_ccs.c`, `lwipopts.h`, `enet_io_ccs.cmd`, `mqtt_io/platform/tm4c1294/*`) + the `.project`/`.cproject` here; relink to `mqtt_io_common/`.
3. **Repoint `mqtt_io_cc35x1/`** linked resources from `mqtt_io/common/…` → `mqtt_io_common/…`. Move root `platform/cc35x1/*` into `mqtt_io_cc35x1/` (CC35x1-specific), including `platform/cc35x1/tools/` → `mqtt_io_cc35x1/tools/` (see the "Tools consolidation" deferred note — keep repo-root `tools/` for host launchers, which reach in via `../`), and any cross-platform seam headers into `mqtt_io_common/`.
4. **Kill duplication:** if the projects can now link `mqtt_io_common/` directly, remove the two-copy build hacks (`fsdata.c`, `wifi_store.c` copies) — see `cc35x1-web-fs-regen` before touching fsdata.
5. **OTA binaries** → single shared `mqtt_io_common/ota/`, filenames `mqtt_io_<platform>_YYYYMMDDHHMM.bin`. Change `OTA_DIR` in `platform/cc35x1/tools/flash.sh` (which moves too) to the shared dir and prefix the platform. Gitignore `mqtt_io_common/ota/`.
6. Delete the emptied `mqtt_io/` dir. Update ALL `mqtt_io/…` / `platform/cc35x1/…` references (makefiles/ORDERED_OBJS, flash scripts, `CLAUDE.md`, docs, `MEMORY.md` pointers).
7. Combine with Plan 7 (portability) and Plan 5 (OTA version) so scripts end up symmetric + parameterized.

## Verification
- BOTH `mqtt_io_tm4c1294` (or `mqtt_io`) and `mqtt_io_cc35x1` build green after **every** step.
- CC35x1 flashes + OTAs; TM4C builds/flashes unchanged.
- Litmus: sketch/prove that a 3rd platform folder would need no edits to `mqtt_io_common/`.

## Do NOT
- Do NOT attempt Option B in one giant commit — step-by-step, green each time, or revert.
- Do NOT hand-edit `.project`/`.cproject` for the rename — use the ccs-project MCP.
- Do NOT touch `fsdata.c` generation without reading `cc35x1-web-fs-regen`.

## Commit
Many small commits (one per verified step) with clear messages + trailer. Ask before push.
