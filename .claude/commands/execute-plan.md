---
description: Execute a plan from docs/plans/ following the standard procedure
argument-hint: PLAN_<name> (e.g. PLAN_web_reboot_freeze_fix)
---

Execute the plan **$ARGUMENTS** from `docs/plans/`.

Follow the "Executing a plan" procedure documented in `docs/plans/README.md` exactly:

1. Read first and confirm you've read them: `C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md`
   (mandatory before ANY ccs-project/ccs-debug/ccs-sysconfig/ccs-serial MCP tool), the repo
   `CLAUDE.md`, `docs/plans/README.md`, the memory index
   `~/.claude/projects/<this-project>/memory/MEMORY.md`, the plan file
   `docs/plans/$ARGUMENTS.md` (or `docs/plans/archive/$ARGUMENTS.md` if it's already archived),
   and every memory/doc that plan links.
2. Execute **only** that plan — its steps / per-file disposition / verification + the global rules.
3. Code/build plans: keep BOTH `mqtt_io_tm4c1294` and `mqtt_io_cc35x1` green via the ccs-project
   MCP `buildProject` after each change set; if a step can't stay green, revert and stop.
   Docs-only plans skip the build gate.
4. Small reviewable commits on `main` with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
   trailer. **Stop for review before committing each step/group.** Do NOT `git push` or flash
   hardware unless I say so. Leave the pre-existing uncommitted
   `platform/cc35x1/{main.c,net_wifi.c,net_wifi.h}` edits untouched and out of your commits.
5. On completion, update the plan's Status in `docs/plans/README.md`; if DONE, `git mv` it to
   `docs/plans/archive/` and move its row to Completed. Refresh affected memories.
6. If anything is ambiguous or conflicts with the plan, STOP and ask rather than guess.
