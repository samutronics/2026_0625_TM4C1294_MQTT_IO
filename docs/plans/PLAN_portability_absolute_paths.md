# Plan 7 — Make the projects build on any PC (remove hardcoded absolute paths)

**Priority:** Med · **Status:** OPEN · Memory: `portability-absolute-paths-todo`

## Goal
Replace machine-specific absolute paths in tracked build files with relative/configurable ones (env vars with current values as defaults), so a fresh checkout builds on any PC. Keep per-platform scripts symmetric (see Plan 6).

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`; memory `portability-absolute-paths-todo`, `build-tooling`.

## Offenders (found 2026-08-24, in tracked files)
1. **User-specific (highest priority):** `platform/cc35x1/tools/prebuild_fs.bat:18` → `set "PY=C:\Users\tomik\AppData\Local\...python3.exe"`.
2. **TI tool roots (per machine, under `C:/ti`):**
   - `platform/cc35x1/tools/flash.sh:37-40` (`SDK`, `TOOLBOX`, `TOOLBOX_EXE`, `GMAKE`)
   - `platform/cc35x1/tools/preflight.sh:36` (`XDSDFU`)
   - `mqtt_io/post_build.ps1:15` (`objcopy` full path)
   - `mqtt_io/.cproject:41-44` (TivaWare include dirs `C:/ti/TivaWare_C_Series-2.2.0.295/...`)
3. **Program Files launchers (low):** `createbin.bat:16`, `flash.bat:15` (`C:\Program Files\Git\bin\bash.exe`).
4. **IDE/tooling config (leave/document):** `.mcp.json` CCS-install launcher — inherently machine-specific.

## Fixes
1. **prebuild_fs.bat** — use `py`/`python` from PATH with an env override:
   `if not defined PY (for %%P in (py.exe python.exe python3.exe) do @if not defined PY where %%P >nul 2>&1 && set "PY=%%P")`. Document `set PY=<path>` override.
2. **Shell/PS1 tool roots** — read from env with current values as DEFAULTS:
   - flash.sh: `CCS_ROOT="${CCS_ROOT:-C:/ti/ccs2100}"`, `SDK="${SDK:-C:/ti/simplelink_wifi_sdk_10_10_01_08}"`, `TOOLBOX="${TOOLBOX:-C:/ti/simplelink_wifi_toolbox_win_4_2_4}"`; derive `GMAKE="$CCS_ROOT/ccs/utils/bin/gmake"`, `TOOLBOX_EXE`, etc.
   - preflight.sh: `XDSDFU="${XDSDFU:-$CCS_ROOT/ccs/ccs_base/common/uscif/xds110/xdsdfu.exe}"` (share `CCS_ROOT`).
   - post_build.ps1: prefer the CCS-provided `$env:CG_TOOL_ROOT`/`$CG_TOOL_ROOT` for objcopy, else an env override, else the current literal as default.
   Keep the SAME env-var names across all platform scripts (symmetry, per Plan 6).
3. **.cproject TivaWare** — do NOT hand-edit. Set a CCS **linked-resource path variable / product reference** (e.g. `TIVAWARE_INSTALL`) via the IDE / ccs-project MCP so the include dirs become `${TIVAWARE_INSTALL}/...`. Prefer built-in `${CG_TOOL_ROOT}` for the compiler.
4. **.bat bash launchers** — try `bash` on PATH first, fall back to the Program Files path.
5. **Do NOT touch generated files** under `Debug/` (`tool_settings.json`, `action_request_extra.txt` — regenerated). `.mcp.json` — document as "regenerate per machine".

## Verification
- Build BOTH projects on a clean checkout. Ideally test on a second machine or with a differently-located `C:/ti`. With defaults unchanged, nothing breaks on the current bench.
- `flash.sh --sign-only` still signs; `prebuild_fs.bat` finds python via PATH.

## Do NOT
- Do not remove the defaults (keep current absolute values as fallbacks).
- Do not hand-edit `.cproject`/`.project`/`.syscfg` (use the CCS MCP).

## Commit
Clear message + trailer; ask before push. Best done alongside Plan 6 so scripts land symmetric.
