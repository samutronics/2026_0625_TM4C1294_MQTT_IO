# Portability & Machine-Specific Path Setup

## Overview
Plan 7 makes the build scripts portable across machines by using environment variables with sensible defaults. This guide explains the remaining manual setup needed for `.cproject` path variables.

## Current Status (2026-09-14)

✅ **Script portability completed:**
- `platform/cc35x1/tools/prebuild_fs.bat` — Python from PATH (or `PY` env var)
- `platform/cc35x1/tools/flash.sh` — TI roots via env vars: `CCS_ROOT`, `SDK`, `TOOLBOX`
- `platform/cc35x1/tools/preflight.sh` — `XDSDFU` via `CCS_ROOT` env var
- `mqtt_io_tm4c1294/post_build.ps1` — objcopy via `$env:CG_TOOL_ROOT` (CCS-provided) or `$env:OBJCOPY`
- `tools/flash.bat`, `tools/createbin.bat` — Bash from PATH or Program Files Git

⚠️ **Pending IDE configuration:**
- `.cproject` include paths still contain absolute paths (TivaWare and workspace-specific)

## What Needs Manual IDE Setup

### TM4C1294 Project (mqtt_io_tm4c1294)

The `.cproject` file contains hardcoded include paths that should be replaced with path variables:

**Current hardcoded paths:**
```
C:/ti/TivaWare_C_Series-2.2.0.295/
C:/ti/TivaWare_C_Series-2.2.0.295/examples/boards/ek-tm4c1294xl
C:/ti/TivaWare_C_Series-2.2.0.295/third_party/lwip-1.4.1/...
C:/Users/tomik/Workspaces/Workspace2026_0625_TM4C1294_MQTT_IO/mqtt_io_common
```

**To make this machine-independent:**

1. **In CCS IDE**, right-click `mqtt_io_tm4c1294` → **Properties**
2. **C/C++ General** → **Path and Symbols**
3. **Under the "Includes" tab:**
   - Create a path variable `TIVAWARE_INSTALL` pointing to your TivaWare location
   - Replace each hardcoded TivaWare path with `${TIVAWARE_INSTALL}/...`
   - Use `${WORKSPACE_LOC}/mqtt_io_common` instead of the absolute workspace path

4. **Click "Apply and Close"**

### Alternatively: Per-User Env Variable (One-Time)

If you prefer not to edit the project repeatedly:

1. **In Windows environment variables**, add:
   ```
   TIVAWARE_INSTALL=C:/ti/TivaWare_C_Series-2.2.0.295
   ```
2. **Restart CCS** to pick up the new env var
3. Update `.cproject` references to use `${env_var:TIVAWARE_INSTALL}`

## Environment Variables Reference

### Build Scripts (already portable)

| Variable | Purpose | Default | Set by |
|----------|---------|---------|--------|
| `CCS_ROOT` | CCS installation directory | `C:/ti/ccs2100` | User env / scripts |
| `SDK` | SimpleLink WiFi SDK path | `C:/ti/simplelink_wifi_sdk_10_10_01_08` | User env / scripts |
| `TOOLBOX` | WiFi Toolbox path | `C:/ti/simplelink_wifi_toolbox_win_4_2_4` | User env / scripts |
| `PY` | Python executable | Probed from PATH | User env / `prebuild_fs.bat` |
| `OBJCOPY` | ARM objcopy tool | `$env:CG_TOOL_ROOT` (CCS) | User env / `post_build.ps1` |
| `BASH` | Git Bash executable | Probed from PATH or Program Files | User env / `.bat` files |

### Usage Examples

**Override SDK location:**
```bash
export SDK=C:/ti/my_custom_sdk_location
./platform/cc35x1/tools/flash.sh
```

**Override Python:**
```cmd
set PY=python
platform\cc35x1\tools\prebuild_fs.bat
```

**Override objcopy in CCS post-build:**
```powershell
$env:OBJCOPY="C:\my\custom\objcopy.exe"
# then build via CCS
```

## Testing Portability

To verify the changes work on a fresh checkout or different machine:

1. **Set environment variables** for your machine paths (if needed)
2. **Build both projects:**
   ```
   buildProject mqtt_io_tm4c1294
   buildProject mqtt_io_cc35x1
   ```
3. **Test flash scripts:**
   ```
   platform\cc35x1\tools\flash.sh --sign-only
   ```

If all scripts run and builds complete, portability is confirmed.

## Notes

- **Defaults remain unchanged:** On the current bench, nothing breaks. Env var defaults match the current setup.
- **Do NOT hand-edit `.cproject` directly** — use the CCS IDE Path & Symbols dialog to maintain consistency.
- **Generated files** under `Debug/` (e.g., `tool_settings.json`) may contain machine-specific paths after rebuild; these are regenerated each build and are not a portability concern.
- `.mcp.json` (CCS AI/MCP launcher) is inherently machine-specific; regenerate it when moving the workspace.
