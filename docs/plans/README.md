# Plans index

One self-contained plan per open task. Each file is written so a smaller model can execute it with no extra context beyond reading the linked prerequisites. **Before executing any plan**, read: `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` (mandatory before any CCS/TI MCP tool), the repo `CLAUDE.md`, and the memory index `~/.claude/projects/<this-project>/memory/MEMORY.md`.

Plan numbers are stable IDs (referenced in commit messages) — they are NOT renumbered when a
plan is archived. When a plan reaches **DONE**, `git mv` its file into [`archive/`](archive/) and
move its row from **Active** to **Completed** below.

### Active

| # | Plan | Area | Priority | Status |
|---|------|------|----------|--------|
| 1 | [PLAN_web_reboot_freeze_fix.md](PLAN_web_reboot_freeze_fix.md) | Bug | High | OPEN |
| 2 | [PLAN_boot_connect_race_fix.md](PLAN_boot_connect_race_fix.md) | Robustness | Low | OPEN |
| 3 | [PLAN_wifi_phaseH_hw_verify.md](PLAN_wifi_phaseH_hw_verify.md) | Verification | High | OPEN |
| 4 | [PLAN_mcu_interchange_rework.md](PLAN_mcu_interchange_rework.md) | Hardware | Med | OPEN (gated) |
| 6 | [PLAN_restructure_perfect_symmetry.md](PLAN_restructure_perfect_symmetry.md) | Structure | Med | PARTIAL (mqtt_io_common extracted; CC35x1 fold + OTA-unify deferred) |
| 7 | [PLAN_portability_absolute_paths.md](PLAN_portability_absolute_paths.md) | Build | Med | OPEN |

### Completed (archived in [`archive/`](archive/))

| # | Plan | Area | Done |
|---|------|------|------|
| 5 | [archive/PLAN_ota_version_fw_timestamp.md](archive/PLAN_ota_version_fw_timestamp.md) | OTA | `51b47cb` (2026-09-01) |
| — | [archive/OTA_FIX_PLAN.md](archive/OTA_FIX_PLAN.md) | OTA | `00395e0` (superseded by Plan 5) |
| 8 | [archive/PLAN_remove_ble_demo.md](archive/PLAN_remove_ble_demo.md) | Cleanup | `1c5258d` (2026-08-24) |
| 10 | [archive/PLAN_tm4c_build_broken_io_fsdata.md](archive/PLAN_tm4c_build_broken_io_fsdata.md) | Bug | DONE |
| 9 | [archive/PLAN_cleanup_root_md_docs.md](archive/PLAN_cleanup_root_md_docs.md) | Cleanup | `18ccebc` (2026-09-04) |

## Executing a plan (procedure used by the `/execute-plan` command)
When asked to execute `PLAN_<name>` (or `/execute-plan PLAN_<name>`):
1. **Read first, confirm:** `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md` (mandatory before any
   CCS/TI MCP tool), the repo `CLAUDE.md`, this `README.md`, the memory index
   `~/.claude/projects/<this-project>/memory/MEMORY.md`, the target plan file (in `docs/plans/`),
   and every memory/doc that plan links. State that you've read them.
2. **Execute that plan only** — its steps / per-file disposition / verification, plus the global
   rules below. Don't start another plan's files.
3. **Green gate (code/build plans):** both `mqtt_io_tm4c1294` and `mqtt_io_cc35x1` must build green
   (via the ccs-project MCP `buildProject`, never raw gmake) after each change set; if a step can't
   stay green, revert it and stop. (Docs-only plans skip the build gate.)
4. **Commits:** small, reviewable, on `main`, each ending with the `Co-Authored-By` trailer.
   **Stop for review before committing each step/group.** Do not `git push` or flash hardware unless
   told. Leave the pre-existing uncommitted `platform/cc35x1/{main,net_wifi}` edits untouched.
5. **On completion:** update the plan's Status here; if DONE, `git mv` it to `archive/` and move its
   row to **Completed**. Refresh any affected memories.
6. If anything is ambiguous or conflicts with the plan, **stop and ask** rather than guess.

**Global rules for every plan:**
- Do ONE plan at a time; build green after each change set; do not start another task's files.
- Never hand-edit `.syscfg`/`.project`/`.cproject` — use the CCS SysConfig / project MCP (CCS.md rule).
- CC35x1 HW verify: COM14 via **pyserial** (not the ccs-serial MCP), and **USB power-cycle** — never a debugger warm reset (wedges the NWP).
- Commits go to `main`; end commit messages with the `Co-Authored-By` trailer. Ask before `git push` unless told otherwise.
- Two-copy gotcha: files that exist in BOTH `platform/cc35x1/` and `mqtt_io_cc35x1/` (e.g. `fsdata.c`, `wifi_store.c`) must be copied to the `mqtt_io_cc35x1/` copy after editing the canonical one.
