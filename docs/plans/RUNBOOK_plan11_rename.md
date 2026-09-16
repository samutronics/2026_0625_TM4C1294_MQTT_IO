# Runbook — Plan 11 CCS-closed directory rename (`mqtt_io_common → iot_foundation`)

> **DONE 2026-09-16 — commit `f3c5f45`.** Executed exactly as below with one deviation
> (chose option (b): hand-edit the CC35x1 generated `.project`/`.cproject` instead of reimport,
> to keep the build steps). One trap not in this doc: after reopening CCS, stale `Debug/*.d`
> dep files still pinned `mqtt_io_common/*.h` → silent `gmake ... not remade because of errors`;
> fix = delete the stale `.d`/`.o` pairs so they regenerate. Both MCUs built green. Kept for
> reference; Scope B/C continue in `PLAN_foundation_product_split.md`.

Operational checklist for the next Plan 11 step. Companion to
[`PLAN_foundation_product_split.md`](PLAN_foundation_product_split.md) (see Phase 0 spike
finding 1 + finding 4) and [`../FOUNDATION_PRODUCT_API_DESIGN.md`](../FOUNDATION_PRODUCT_API_DESIGN.md).

**Starting point:** `main` @ `5c75c6c` (the three in-place cleaves —
`mqtt_app`/`webui`/`config` — are landed and HW-verified). No cleave work remains before the rename.

## Why this must be a CCS-CLOSED session (spike finding 1)

`git mv mqtt_io_common …` fails with *Permission denied* while CCS is open — CCS holds the
shared files locked as **linked resources** in both live projects. And the CCS MCP build
servers die when CCS closes, so **you cannot build-verify inside the move**. Consequence:

- The move + path find/replace is done with **CCS fully closed** (plain git/file edits).
- This is the ONE sanctioned exception to "never hand-edit `.project`/`.cproject`" — the CCS
  project MCP's `renameProject` renames a *project*, not a shared linked-resource **directory**,
  so it cannot perform this rename. Hand-edit the path strings.
- **Build verification is a separate step:** the user reopens CCS afterward; the assistant then
  drives `buildProject` for both MCUs via the MCP.

## Scope decision (do the cheap rename FIRST; defer the file-splitting)

This runbook covers **Scope A only** — the pure directory rename. Two later scopes are explicitly
deferred so Scope A stays a small, build-verifiable, revertible step:

| Scope | What | When |
|---|---|---|
| **A — dir rename (THIS runbook)** | `git mv mqtt_io_common iot_foundation`; update the path-bearing files below. Every file keeps its current relative position under the new dir name. | Now (CCS-closed) |
| B — move whole-file product TUs | `git mv` the clean product files (`common/{io_scan,output_ctrl,relay_pulse,input_events}.{c,h}`, `din_chain.*`, `relay_chain.*`, `fs/{iocfg,control,iostate}.shtml`) into `products/home_auto/`. Adds member-path churn (not new TUs). | After A builds green |
| C — split the dual-half files | Cut the marked **PRODUCT sections** of `mqtt_app.c`/`webui.c`/`config.c` into separate `products/home_auto/…` TUs. **Adds new .c files → real project surgery on both MCUs** (projectspec file entries + TM4C `.project` linked resources). | Phase 3 (its own session) |

Rationale: A is a rename that any tool can verify; C changes the TU set and is where the risk lives.
Keep them apart.

## Scope A — exact edit targets

Tracked files that reference `mqtt_io_common` (from `git grep`, excluding `Debug/` build artifacts,
`.git/`, and the plan docs that *describe* the plan — leave `docs/plans/*` and `docs/FOUNDATION_*`
prose alone):

| File | Refs | Notes |
|---|---|---|
| `mqtt_io_tm4c1294/.project` | 14 | **Tracked, hand-owned.** Linked-resource `<location>`/`<locationURI>` paths. Hand-edit. |
| `mqtt_io_tm4c1294/.cproject` | 3 | **Tracked.** Include paths (`mqtt_io_common`, `/common`, `/pal`). Hand-edit. |
| `platform/cc35x1/mqtt_io_cc35x1.projectspec` | 17 | **Tracked — the CC35x1 source of truth.** 3 include paths + ~15 `<file>` entries (mixed `action="link"`/`"copy"`). Hand-edit. |
| `platform/cc35x1/tools/prebuild_fs.bat` | 2 | Reads `mqtt_io_common\fs`. Update to `iot_foundation\fs`. |
| `.claude/settings.json` | 1 | Path allow-rule / ref. |
| `.claude/settings.local.json` | 3 | Path allow-rules. |
| `README.md` | 2 | Prose/paths. |
| `docs/PORTABILITY.md` | 2 | Prose/paths. |

**Gitignored, do NOT hand-track:** `mqtt_io_cc35x1/.project` + `.cproject` are **generated from the
projectspec**. After editing the projectspec, either (a) reimport the CC35x1 project so they
regenerate — **then RE-ADD the post-build flash step + pre-build fsdata step** (reimport drops manual
build steps; see the `tools-dir-and-buildstep-paths` + `rename-tm4c-project-todo` memories), or
(b) if not reimporting, hand-edit the two generated files in place so the live project still resolves.
Prefer (a) for a clean regeneration; budget for re-adding the build steps.

## Procedure (CCS closed)

1. **Confirm CCS is fully closed** (no `theia`/CCS processes holding locks). Confirm clean-ish tree:
   the only expected dirty files are the pre-existing WIP (`platform/cc35x1/main.c`,
   `mqtt_io_common/fs/index.shtml`, `mqtt_io_tm4c1294/{enet_io.c,io_fsdata.h}`,
   `platform/cc35x1/fsdata.c`) — decide whether to stash them first.
2. `git mv mqtt_io_common iot_foundation` (single move; preserves history).
3. Find/replace `mqtt_io_common` → `iot_foundation` in the 8 files above. Do it per-file and eyeball
   each hunk — these are project/build files, not free text.
4. Update `prebuild_fs.bat` and re-verify the CC35x1 two-copy fsdata path
   (`cc35x1-web-fs-regen` memory): source is now `iot_foundation\fs`.
5. For the CC35x1 project dir: reimport from the updated projectspec **or** hand-fix the generated
   `.project`/`.cproject`; then re-add build steps if reimported.
6. `git status` — verify only intended path changes; nothing unexpected staged.

## Verification gate (user reopens CCS)

7. User reopens CCS. Assistant runs the **CC35x1 debug/flash preflight** from `CLAUDE.md`
   (`getActiveProjectName` must be `mqtt_io_cc35x1` before any CC35x1 action).
8. `buildProject mqtt_io_tm4c1294` → green. `buildProject mqtt_io_cc35x1` → green (post-build
   auto-flash runs). If a project fails to resolve a linked resource / include, fix the path and
   rebuild — **do not** proceed to commit until both are green.
9. Optional HW smoke: web UI loads (base + I/O tabs), OTA reachable — proves fsdata path + includes.
10. Commit as `refactor(plan11): rename mqtt_io_common → iot_foundation (CCS-closed)`, then update
    `PLAN_foundation_product_split.md` status + the `foundation-product-split-plan` memory.

## Gotchas (carry in)

- **Never hand-edit `.syscfg`** — untouched by this rename anyway.
- Reimport **drops** the CC35x1 post-build flash + pre-build fsdata steps — re-add them.
- CC35x1 two-copy fsdata: HTML reaches firmware only via the regen path (`cc35x1-web-fs-regen`).
- The CR-in-CCS-Pre-build-field trap (`tools-dir-and-buildstep-paths` memory): if you re-add build
  steps, don't let a stray CR into the `.cproject` macro.
- Leave the pre-existing WIP files untouched (or stash → restore around the move).
- If anything can't be made green, **revert the move and stop** — the rename must land build-green.
