# Claude Workflow --- Token-Efficient Project Rules

## Purpose

This file is a workflow layer for Claude Code. It complements
`CLAUDE.md` and the project architecture/design documents.

Goals: 1. Minimize unnecessary context, tool calls, rereads, and
generated text. 2. Preserve correctness and hardware safety. 3. Prefer
existing project patterns over new abstractions. 4. Escalate reasoning
effort only when the task actually requires it.

## 1. Context discipline

-   Read `CLAUDE.md` first for CCS/TI tasks.
-   Read only documents and source files relevant to the current task.
-   Do not broadly explore the repository unless the task genuinely
    requires architecture discovery.
-   Start from the named function/module/feature and trace outward only
    as needed.
-   Do not reread files when their relevant contents are already in
    context.
-   Prefer targeted symbol/search commands before opening large files.
-   Batch independent reads where practical.
-   Do not reproduce large file contents in responses.

Check existing project documentation before rediscovering
architecture: - `CC35X1_PORTING_PROPOSAL.md` -
`CC35X1_BLE_PROVISIONING.md` - `CLAUDE.md` - other project-specific
design notes

Treat documented decisions as the default unless current source code or
hardware evidence contradicts them.

## 2. Exploration mode

For a new or unfamiliar feature:

1.  Identify the closest existing implementation.
2.  Find the smallest dependency path needed.
3.  Verify uncertain SDK/API/hardware capabilities before designing
    around them.
4.  Make a short implementation plan.
5.  Implement unless analysis-only was requested.

Target:

> Find the minimum set of files and symbols needed to make the change
> safely.

Do not do "understand the entire codebase" exploration when the
architecture is already documented.

## 3. Implementation mode

Prefer the smallest correct change.

-   Reuse existing interfaces and patterns.
-   Do not add an abstraction unless it is required for portability,
    testing, or a real architectural boundary.
-   Do not refactor unrelated code.
-   Preserve existing behavior unless explicitly asked to change it.
-   Follow the established `common/` / `pal/` / `platform/<name>/`
    architecture.
-   Keep CC35x1 vendor details inside `platform/cc35x1/` or established
    PAL/OSAL seams.
-   Do not fork shared logic merely because platforms differ.

## 4. Tool efficiency

### Reads and searches

-   Search first; open second.
-   Prefer symbol/function searches over whole-file reads.
-   Batch independent reads.
-   Stop searching once the relevant code path is known.
-   Do not inspect generated/vendor files unless directly relevant.
-   If a tool fails twice for the same reason, change strategy.
-   If an MCP is unavailable, use safe local files/CLI alternatives.

### Edits

-   Group related edits.
-   Avoid read → edit → reread cycles unless verification is meaningful.
-   Build after a coherent implementation, not after every trivial edit.
-   Fix related compilation errors in batches.

### Todos

Use at most 5 todo items. Group work by outcome, not file.

Preferred: 1. Inspect/design 2. Implement 3. Integrate 4. Build/test 5.
Verify/release

Do not update todos for every small edit.

## 5. Output and verbosity

Default style: concise and operational.

Do not narrate routine actions such as: - "I'll now inspect..." - "Let
me check..." - "Now I'll update..." - repeated explanations of the same
decision

Do not provide running commentary during tool use.

For normal implementation tasks, final output should contain only:

-   **Changed:** files/modules
-   **Result:** build/test result
-   **Issues:** blockers or important caveats
-   **Next:** one concrete next action, if needed

Target roughly 5--15 lines for routine tasks.

Give detailed explanations only when explicitly requested, when an
architectural decision is non-obvious, when diagnosing a failure, or
when there is a meaningful hardware/safety risk.

Do not quote source code unless a small excerpt is necessary.

## 6. Plan versus implementation

Do not spend a long conversation in planning mode.

If the requested behavior and integration point are clear: - inspect
enough to confirm the design; - implement.

If genuinely ambiguous: - identify the decision; - give 2--3 concise
options; - recommend one; - proceed when authorized.

Do not create a large plan for a small change.

## 7. Verification

Before claiming success:

1.  Build the affected target.
2.  Run the smallest relevant tests/checks.
3.  Inspect the actual result.
4.  For hardware, distinguish clearly between build, flash, and runtime
    verification.

Never claim hardware behavior was verified when only compilation or
flashing succeeded.

Do not run long serial monitors, polling loops, or waits for the user
unless explicitly requested. If a physical action is required, stop and
report the exact action.

## 8. CCS / hardware efficiency

For CC35x1 work, obey the preflight rules in `CLAUDE.md`.

In particular: - confirm the active project before CC35x1 debugging; -
confirm `APP_MCU` / M33 after connecting; - use the project flash script
and probe preflight; - do not use raw DSLite when the project script
exists.

When debug/serial/flash fails, do not blindly retry. First determine
whether the cause is project/core selection, probe identity, power/reset
state, CCS/MCP state, or application state.

Prefer one diagnostic step that distinguishes these cases.

## 9. Generated files

Before modifying generated files, identify their source and regeneration
command.

Prefer changing the source/template and regenerating. Follow existing
project conventions for tracked/generated outputs.

## 10. Hardware and SDK uncertainty

Verify capability first, implement second.

Use installed headers, examples, SysConfig output, existing project
code, or SDK documentation.

Do not assume a familiar API from another TI device exists on CC35x1.

If an implementation depends on an uncertain capability, identify the
uncertainty before making a large change.

## 11. Model / reasoning escalation

Use the least expensive model/reasoning level likely to be correct.

Use a fast/cheap model for: - targeted searches; - extracting facts; -
mechanical edits; - repetitive declarations; - formatting; -
straightforward build-error fixes; - concise summaries.

Use the normal/default coding model for: - ordinary implementation; -
integration across a few modules; - tests; - routine debugging.

Escalate to the strongest reasoning model for: - new architecture; -
difficult RTOS/concurrency issues; - unclear hardware behavior; - subtle
SDK behavior; - major refactors; - regressions that resist
straightforward diagnosis; - high-risk release decisions.

Do not spend maximum reasoning on mechanical work.

## 12. Session boundaries

Prefer short, purposeful sessions.

End a session when: - implementation is complete and only hardware
verification remains; - the user must perform a physical action; - a
feature milestone is complete; - context is dominated by obsolete
exploration/tool output.

Leave a compact state note:

``` text
Task:
Status:
Changed:
Build/test:
Hardware status:
Known issues:
Next action:
```

Do not carry an entire historical conversation forward when a short
state summary is sufficient.

## 13. Compaction

Use `/compact` at meaningful milestones.

When compacting, preserve only: - current task; - architecture
decisions; - constraints; - files/functions changed; - build/test
status; - hardware state; - unresolved questions; - exact next action.

Discard routine tool narration and obsolete exploration.

## 14. Settled decisions

Do not repeatedly explain or reopen decisions already documented.

Current CC35x1 decisions include: - FreeRTOS is the selected RTOS. -
Single monorepo with `common/`, `pal/`, and `platform/<name>/`. - BLE
provisioning is the current CC35x1 onboarding path. - Shared
MQTT/web/domain logic should remain common. - CC35x1 hardware access
belongs behind the platform/PAL boundary.

Revisit these only when new source, SDK, or bench evidence changes them.

## 15. Completion format

For implementation:

``` text
Changed: <files/modules>
Result: <build/test result>
Issues: <none or concise blocker>
Next: <one action, if applicable>
```

For analysis:

``` text
Finding: <one-sentence conclusion>
Relevant: <files/functions>
Risk: <none or concise risk>
Recommendation: <one concrete recommendation>
```

Avoid whole-conversation summaries unless explicitly requested.

## Core rule

> Do the minimum investigation, make the minimum correct change, verify
> it once, and report the result briefly.
