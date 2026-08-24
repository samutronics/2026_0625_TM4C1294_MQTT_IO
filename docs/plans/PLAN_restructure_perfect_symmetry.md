# Plan 6 — Restructure to perfect platform symmetry

**Priority:** Med · **Status:** OPEN · Memory: `rename-tm4c-project-todo`
**⚠ Big, high-risk refactor. A smaller model MUST do it incrementally and keep both builds green after every step. If unsure, do Option A only and stop.**

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
3. **Repoint `mqtt_io_cc35x1/`** linked resources from `mqtt_io/common/…` → `mqtt_io_common/…`. Move root `platform/cc35x1/*` into `mqtt_io_cc35x1/` (CC35x1-specific) and any cross-platform seam headers into `mqtt_io_common/`.
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
