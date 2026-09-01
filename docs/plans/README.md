# Plans index

One self-contained plan per open task. Each file is written so a smaller model can execute it with no extra context beyond reading the linked prerequisites. **Before executing any plan**, read: `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` (mandatory before any CCS/TI MCP tool), the repo `CLAUDE.md`, and the memory index `~/.claude/projects/<this-project>/memory/MEMORY.md`.

| # | Plan | Area | Priority | Status |
|---|------|------|----------|--------|
| 1 | [PLAN_web_reboot_freeze_fix.md](PLAN_web_reboot_freeze_fix.md) | Bug | High | OPEN |
| 2 | [PLAN_boot_connect_race_fix.md](PLAN_boot_connect_race_fix.md) | Robustness | Low | OPEN |
| 3 | [PLAN_wifi_phaseH_hw_verify.md](PLAN_wifi_phaseH_hw_verify.md) | Verification | High | OPEN |
| 4 | [PLAN_mcu_interchange_rework.md](PLAN_mcu_interchange_rework.md) | Hardware | Med | OPEN (gated) |
| 5 | [PLAN_ota_version_fw_timestamp.md](PLAN_ota_version_fw_timestamp.md) | OTA | Med | OPEN |
| 6 | [PLAN_restructure_perfect_symmetry.md](PLAN_restructure_perfect_symmetry.md) | Structure | Med | OPEN |
| 7 | [PLAN_portability_absolute_paths.md](PLAN_portability_absolute_paths.md) | Build | Med | OPEN |
| 8 | [PLAN_remove_ble_demo.md](PLAN_remove_ble_demo.md) | Cleanup | Low | DONE |
| 9 | [PLAN_cleanup_root_md_docs.md](PLAN_cleanup_root_md_docs.md) | Cleanup | Low | OPEN |
| 10 | [PLAN_tm4c_build_broken_io_fsdata.md](PLAN_tm4c_build_broken_io_fsdata.md) | Bug | High | DONE |

**Global rules for every plan:**
- Do ONE plan at a time; build green after each change set; do not start another task's files.
- Never hand-edit `.syscfg`/`.project`/`.cproject` — use the CCS SysConfig / project MCP (CCS.md rule).
- CC35x1 HW verify: COM14 via **pyserial** (not the ccs-serial MCP), and **USB power-cycle** — never a debugger warm reset (wedges the NWP).
- Commits go to `main`; end commit messages with the `Co-Authored-By` trailer. Ask before `git push` unless told otherwise.
- Two-copy gotcha: files that exist in BOTH `platform/cc35x1/` and `mqtt_io_cc35x1/` (e.g. `fsdata.c`, `wifi_store.c`) must be copied to the `mqtt_io_cc35x1/` copy after editing the canonical one.
