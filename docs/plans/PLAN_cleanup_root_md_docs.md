# Plan 9 — Clean up the root-level .md task/plan files

**Priority:** Low · **Status:** OPEN · Memory: `cleanup-root-md-docs-todo`

## Goal
Move Claude task/plan `.md` files out of the workspace root into a tidy `docs/` tree, keeping only conventional docs at root. Delete truly-closed plans (git history preserves them).

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`; memory `cleanup-root-md-docs-todo`.

## Target layout (aligned to the current tree, 2026-09-04)
- **Root (keep only):** `README.md`, `CLAUDE.md`, `HARDWARE.md`.
- **`docs/`** — living reference docs (design proposals, workflow, porting notes) + `docs/pdf/` schematics.
- **`docs/plans/`** — ACTIVE plans + the `README.md` index (already split into **Active** / **Completed**).
- **`docs/plans/archive/`** — CLOSED plans (folder already exists; the plans `README.md` **Completed**
  table is the index — one row each with the delivering commit). Optionally add a
  `> **Status: CLOSED**` banner atop each archived file. There is **no** separate `docs/archive/`.

## Per-file disposition
| File | Action |
|------|--------|
| `README.md`, `CLAUDE.md`, `HARDWARE.md` | keep at root |
| `CC35X1_PORTING_PROPOSAL.md`, `CC35X1_BLE_PROVISIONING.md` | `git mv` → `docs/` (reference) |
| `CLAUDE_WORKFLOW.md` | classify: `docs/` if reference, else `docs/plans/archive/` |
| `MCU_INTERCHANGE_PLAN.md` | reference doc → `git mv` → `docs/` (the actionable plan is `docs/plans/PLAN_mcu_interchange_rework.md`) |
| `WIFI_FEATURES_PLAN.md` | `git mv` → `docs/plans/` now; on close → `docs/plans/archive/` |
| `OTA_FIX_PLAN.md` | CLOSED → `git mv` → `docs/plans/archive/` (or delete — captured in code + memory + `archive/PLAN_ota_version_fw_timestamp.md`) |

Note: `pdf/` → `docs/pdf/` and the DONE plans (5/8/10) → `docs/plans/archive/` are already done.

## Steps
1. `git mv` each remaining root file to its destination (preserves history). Closed plans go to
   `docs/plans/archive/` and get a row in the plans `README.md` **Completed** table (no separate
   archive index file needed).
2. **Fix references after moving** — grep the repo for the moved filenames and update paths:
   ```
   grep -rIn "OTA_FIX_PLAN.md\|WIFI_FEATURES_PLAN.md\|MCU_INTERCHANGE_PLAN.md\|CC35X1_PORTING_PROPOSAL.md\|CC35X1_BLE_PROVISIONING.md\|CLAUDE_WORKFLOW.md" . --include=*.md --include=*.sh --include=*.bat
   ```
   Update the plan files in `docs/plans/` that cite these, their handoff prompts, and the `MEMORY.md` pointers.
3. Add a one-line convention to `CLAUDE.md`: "New plan docs live in `docs/plans/`; on close, `git mv`
   to `docs/plans/archive/` and move the index row to Completed, or delete once captured in code + memory."
   (The plans `README.md` already documents this workflow.)

## Deletion policy
`docs/plans/archive/` is a staging area for closed-but-still-referenced plans. Once a plan's outcome
is in shipped code + a memory + README, **delete it** (`git show <commit>:<path>` recovers it any time).

## Verification
- Root contains only README/CLAUDE/HARDWARE (+ this repo's non-doc files). `docs/` + `docs/plans/` +
  `docs/plans/archive/` populated; all internal links resolve (grep shows no dangling references to old root paths).

## Do NOT
- Do not put these under `.claude/` (harness config, partly user-specific).
- Do not create a separate `docs/archive/` — closed **plans** live in `docs/plans/archive/`.
- Do not delete `CLAUDE.md`/`README.md`/`HARDWARE.md`.

## Kickoff prompt (paste into a fresh session)
```
Plan 9 — Clean up root-level .md docs

Read docs/plans/README.md, then docs/plans/PLAN_cleanup_root_md_docs.md, then
C:/ti/ccs2100/ccs/theia/resources/ai/CCS.md, the repo CLAUDE.md, and the memory index
~/.claude/projects/<this-project>/memory/MEMORY.md plus the memory it links
(cleanup-root-md-docs-todo). Confirm you've read them.

Execute Plan 9 only, following its steps / per-file disposition / verification and the
global rules in the plans README. Key points:

  - This is a docs-only reorg (git mv, no code/build changes) — but still do NOT hand-edit
    .project/.cproject/.syscfg, and don't touch either project's sources.
  - Move with `git mv` (preserve history). Respect the settled convention: living reference
    docs → docs/ ; ACTIVE plans → docs/plans/ ; CLOSED plans → docs/plans/archive/ (NOT a
    separate docs/archive/). The plans README "Completed" table is the archive index — add a
    row (with the delivering commit) for anything you archive; optionally add a
    `> **Status: CLOSED**` banner atop each archived file.
  - After every move, grep the repo (*.md, *.sh, *.bat, *.ps1) for the moved filenames and
    fix all references, including handoff prompts inside plans and the MEMORY.md pointers.
    Prove no dangling links remain (grep clean).
  - Classify the ambiguous ones per the plan table (e.g. MCU_INTERCHANGE_PLAN.md → docs/ as
    reference; CLAUDE_WORKFLOW.md → docs/ if reference else archive). If a file's disposition
    is genuinely unclear, ask me rather than guess.
  - Add the one-line "new plans → docs/plans/, on close → docs/plans/archive/ or delete"
    convention to CLAUDE.md (user-instructions section, below the auto-generated part).
  - Do the moves in small, reviewable git commits grouped sensibly (e.g. reference docs;
    plans; closed plans), each with the Co-Authored-By trailer.

Stop for my review before committing each group, and do not git push unless I say so.
Leave the already-uncommitted platform/cc35x1/{main.c,net_wifi.c,net_wifi.h} changes
untouched and out of your commits. If anything conflicts with the plan or is ambiguous,
stop and ask.
```
