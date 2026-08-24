# Plan 9 — Clean up the root-level .md task/plan files

**Priority:** Low · **Status:** OPEN · Memory: `cleanup-root-md-docs-todo`

## Goal
Move Claude task/plan `.md` files out of the workspace root into a tidy `docs/` tree, keeping only conventional docs at root. Delete truly-closed plans (git history preserves them).

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`; memory `cleanup-root-md-docs-todo`.

## Target layout
- **Root (keep only):** `README.md`, `CLAUDE.md`, `HARDWARE.md`.
- **`docs/`** — living reference.
- **`docs/plans/`** — ACTIVE plans (this folder already exists with the per-task plans + `README.md` index).
- **`docs/archive/`** — CLOSED/superseded plans, with `docs/archive/README.md` index (one line each: what it was, when closed, delivering commit) and a `> **Status: CLOSED**` banner atop each archived file.

## Per-file disposition (2026-08-24)
| File | Action |
|------|--------|
| `README.md`, `CLAUDE.md`, `HARDWARE.md` | keep at root |
| `CC35X1_PORTING_PROPOSAL.md`, `CC35X1_BLE_PROVISIONING.md` | `git mv` → `docs/` |
| `CLAUDE_WORKFLOW.md` | classify: `docs/` if reference, else `docs/archive/` |
| `MCU_INTERCHANGE_PLAN.md` | `git mv` → `docs/plans/` (open) |
| `WIFI_FEATURES_PLAN.md` | `git mv` → `docs/plans/` now; move to `docs/archive/` once HW-verified |
| `OTA_FIX_PLAN.md` | CLOSED → `git mv` → `docs/archive/` (can be deleted after one more confidence cycle) |

## Steps
1. `git mv` each file to its destination (preserves history). Create `docs/archive/README.md` index.
2. **Fix references after moving** — grep the repo for the moved filenames and update paths:
   ```
   grep -rIn "OTA_FIX_PLAN.md\|WIFI_FEATURES_PLAN.md\|MCU_INTERCHANGE_PLAN.md\|CC35X1_PORTING_PROPOSAL.md\|CC35X1_BLE_PROVISIONING.md" . --include=*.md --include=*.sh --include=*.bat
   ```
   Update the plan files in `docs/plans/` (they cite the root docs), and the handoff prompts inside them. Update the `MEMORY.md` pointers that name these paths.
3. Add a one-line convention to `CLAUDE.md`: "New plan docs live in `docs/plans/`; on close, move to `docs/archive/` or delete once captured in code + memory."

## Deletion policy
Archive is only a staging area for closed-but-still-referenced docs. Once a plan's outcome is in shipped code + a memory + README, **delete it** (`git show <commit>:<path>` recovers it any time).

## Verification
- Root contains only README/CLAUDE/HARDWARE (+ this repo's non-doc files). `docs/` tree populated; all internal links resolve (grep shows no dangling references to old root paths).

## Do NOT
- Do not put these under `.claude/` (harness config, partly user-specific).
- Do not delete `CLAUDE.md`/`README.md`/`HARDWARE.md`.
