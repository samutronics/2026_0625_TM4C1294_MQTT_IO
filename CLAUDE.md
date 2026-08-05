<!-- DO NOT EDIT - This part is automatically generated. -->

# Agent Guidelines

## CCStudio IDE Installation Directory

CCStudio IDE is installed at `C:/ti/ccs2100`. Save it as `{ccs-install-dir}` for the session — scripts and tools will need it.

## MANDATORY Pre-Task Requirement (DO NOT SKIP)

**CRITICAL - NO EXCEPTIONS**: Before ANY CCS/Texas Instruments-related task (even simple ones), you MUST read `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md`. This file includes information on how to interact with CCS as well as device-specific information (UART backchannel pins, LED setup, transmit best practices, etc.). 

Do NOT call any ccs-project, ccs-debug, ccs-sysconfig, or ccs-serial MCP tools until CCS.md has been read.


<!-- DO NOT EDIT - This part is automatically generated. -->

<!-- User instructions should be added below this line -->

## CC35x1 Debug/Flash Preflight (prevents the M4/M33 target mix-up)

The TM4C project `mqtt_io` (Cortex-**M4**) and the CC35x1 project `mqtt_io_cc35x1`
(Cortex-**M33**) share this workspace. `debugProject` has no project argument — it
uses the CCS **active project** — so if a TM4C build ran last, a CC35x1 debug launch
silently attaches to the M4 and fails with an "M4 error". Enforce these invariants:

**Debug (CCS MCP):** before every `debugProject` for the CC35x1:
1. `getActiveProjectName` **must** be `mqtt_io_cc35x1`. If not, run
   `buildProject mqtt_io_cc35x1` (this flips the active project), then re-check.
2. After connecting, `listCores` **must** report `APP_MCU` (the M33). If you see an
   M4/TM4C core, `terminate`, fix the active project, and retry. Trust nothing until
   `APP_MCU` is confirmed.

**Flash (shell):** always flash via `platform/cc35x1/tools/flash.sh`, which sources
`preflight.sh` to assert exactly one XDS110 present and its SN matches the pinned
CC35x1 bench probe (`CC35_PROBE_SN`, default `E10000H6`). Never use raw DSLite. The
TM4C's Stellaris-ICDI probe does not enumerate under `xdsdfu`, so the flash path
cannot reach the M4 by construction. Run `preflight.sh` standalone anytime to
re-print the debug checklist and re-verify probe identity.

