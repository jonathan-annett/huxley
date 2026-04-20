# EMU-12 Triage Brief

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find `tasks/emu12-task.md` in the repository. That is the *original* EMU-12 spec that a previous agent was working from. Phase 1 of this brief asks you to read it for reference, so you understand what the prior work was supposed to produce. It is **not** your assignment. Your assignment is this document — the triage brief — and nothing else.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86`, later to be paired with a browser-based editor. The emulator is in `packages/emu86/`. The project uses a task-based workflow: one task per commit, task files in `tasks/`, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

Tasks EMU-01 through EMU-11 are committed and clean. EMU-12 (Linux host platform and CLI entry point) is **uncommitted, in working-tree state, and its completion status is disputed**. That is what you are triaging.

**Do not read the project plan or emu86 roadmap for this task.** You don't need the broader vision. Your scope is narrow: figure out what's going on with EMU-12 and either fix it or report.

## What you are NOT doing

- You are **not** implementing EMU-13 or anything further.
- You are **not** modifying the project plan, the build plan, or any proposal documents.
- You are **not** fixing things outside the EMU-12 scope even if you notice them. Note them in your report, don't touch them.
- You are **not** committing anything until explicitly told by the user, via chat, after you report.

## The evidence so far

A prior Claude Code session worked on EMU-12 and produced unit tests that pass (2876 assertions, no failures). It then updated the task log claiming `PASS (unit tests) — manual boot test pending` and left the tree uncommitted. The user's session was interrupted for ~4 weeks before resuming.

On resumption, review of the uncommitted state revealed:

1. **Debug `fprintf(stderr, ...)` statements left in `packages/emu86/src/emulator/run.c`** — specifically a trace block that prints CPU state, decoded xlat_id, and IP-advance info for the first 200 instructions. Added inside `emu86_run()`. Also a `CASE8` debug print inside `execute_instruction()`. These are unconditional (not `#ifdef` guarded) and are clearly diagnostic, not production code.

2. **Debug `fprintf(stderr, ...)` statements left in `packages/emu86/src/hosts/linux/main.c`** — three `DEBUG:` labelled prints at approximately lines 207, 211, and 220, dumping CPU state, memory bytes at F0100, and yield/cycle info during execution.

3. **Boot tests marked "PENDING"** in the task log, with no evidence of a successful boot ever having occurred.

The interpretation the user and I arrived at: the prior agent attempted a boot (FreeDOS, or the reference build, or both), it didn't work, the agent instrumented the run loop and host entry to diagnose, and then the session ended before the bug was resolved. The "boot tests pending" framing in the task log is misleading — it suggests the tests simply weren't run, when evidence indicates they were attempted and failed.

**Treat this interpretation as a hypothesis, not a fact.** It is possible the debug code was added earlier for a different reason, or that boot actually works and the debug code is stale scaffolding from a previous problem. Verify, don't assume.

## Files in the uncommitted tree

Modified:
- `.gitignore`
- `packages/emu86/.claude/settings.local.json`
- `packages/emu86/Makefile`
- `packages/emu86/src/emulator/run.c` (contains debug traces)
- `tasks/completed/task-log.md`

Untracked:
- `packages/emu86/docs/editor-api-proposal.md` (unrelated to this triage; don't touch)
- `packages/emu86/src/hosts/linux/main.c` (contains debug traces)
- `packages/emu86/src/hosts/linux/platform_linux.c`
- `packages/emu86/src/hosts/linux/platform_linux.h`
- `packages/emu86/test/integration/test_boot_freedos.sh`
- `packages/emu86/test/unit/test_platform_linux.c`
- `tasks/TASK-FOOTER.md`
- `tasks/emu12-task.md`

The test disk images should be in `packages/emu86/test/images/` — `freedos.img` is expected to exist from EMU-01. Verify before attempting a boot.

## Your task

### Phase 1 — Assess (no code changes yet)

1. Read `tasks/emu12-task.md` to understand what EMU-12 was *supposed* to produce.
2. Read the debug traces in `run.c` and `main.c` and work out what the agent was investigating. Specifically: what was it trying to print, at what points in execution, and what would a failing boot likely look like in the trace output?
3. Look at the `Makefile` diff to understand what build target is meant to produce the `emu86` binary and how the boot script (`test_boot_freedos.sh`) is meant to invoke it.
4. Confirm that `packages/emu86/test/images/freedos.img` exists and is readable.
5. Run the unit test suite and confirm it passes:
   ```
   cd packages/emu86 && make test-unit
   ```
   If this doesn't pass, stop here and report — something is broken beyond the scope we thought.

### Phase 2 — Reproduce

With the debug code **still in place**, build and run the emulator against the FreeDOS image. Collect the trace output. Do not redirect away stderr — you need to see the debug spew.

```
cd packages/emu86
make
./emu86 reference/bios test/images/freedos.img 2>&1 | head -400
```

(Adjust argument order if the actual `usage:` string in `main.c` differs. Read `print_usage()` in `main.c` before running.)

Record what you see. Three possible outcomes:

**(a) FreeDOS prompt appears** — boot works. The debug traces were leftover from an earlier problem that got fixed, and the prior agent forgot to remove them. Go to Phase 3a.

**(b) The emulator fails deterministically** — crashes, infinite loops, prints garbage, hits an unknown opcode, diverges from expected CPU state. This is the bug the prior agent was chasing. Go to Phase 3b.

**(c) Something else** — non-deterministic behaviour, build failures, missing dependencies. Stop and report without proceeding to Phase 3.

### Phase 3a — Clean up (only if boot works)

1. Remove the debug blocks from `run.c`:
   - The `TRACE` block inside `emu86_run()` (around line 566)
   - The `xlat_id` debug print (around line 586)
   - The `IP advance` / `IP jumped` debug prints (around lines 598–603)
   - The `CASE8` debug print inside `execute_instruction()` (around line 195)
   - Remove `#include <stdio.h>` from the top of `run.c` if and only if nothing else in the file uses it after cleanup.

2. Remove the three `DEBUG:` prints from `main.c` (around lines 207, 211, 220). Keep all other `fprintf(stderr, ...)` calls — those are legitimate error messages and usage output.

3. Rebuild, re-run the unit tests, re-run the boot test. All three must succeed.

4. Write your report (see Phase 4). Do **not** commit. The user will review and commit.

### Phase 3b — Diagnose (if boot fails)

1. Characterise the failure precisely. What is the last-known-good instruction? What does the CPU state look like at the failure point? Is it:
   - An instruction-decoding bug (wrong `xlat_id`, wrong `inst_length`)?
   - An instruction-execution bug (wrong flag computation, wrong memory access)?
   - A BIOS/boot-loading bug (BIOS not at the right address, boot sector not loaded, segment registers wrong at entry)?
   - A platform-callback bug (disk read returning wrong data, timer misbehaving)?
   - A host-init bug (something in `main.c`'s setup sequence doesn't match what the original 8086tiny did)?

2. Cross-reference against the reference build: compile and run `reference/8086tiny` with the same FreeDOS image. If the reference boots and ours doesn't, diff the behaviour. The agent who did EMU-01 produced `docs/ORIGINAL-ANALYSIS.md` — consult it if needed for init-sequence details.

3. Attempt a fix **only if** the bug is clearly localised and fixable within EMU-12's scope (host/platform/init code, or a specific opcode bug that can be isolated with a targeted unit test). If the bug is anywhere else — decoder, opcode family, run loop structure — **do not fix it here**. That's a scope violation. Report the bug location and stop.

4. If you fix a bug, add a regression unit test that would have caught it. Strip debug code. Rebuild. Re-run unit tests AND boot test. Both must pass.

5. Write your report. Do not commit.

### Phase 4 — Report

Write a markdown report to `tasks/triage/emu12-triage-report.md` (create the `tasks/triage/` directory if it doesn't exist). Structure:

```markdown
# EMU-12 Triage Report

Date: <today>
Outcome: <CLEAN / FIXED / BLOCKED>

## Summary
<2-3 sentences on what you found>

## Evidence
<what the debug traces showed, what the boot test showed, what the unit tests showed>

## Root cause
<if a bug existed: exactly what it was, where, and why>
<if no bug: what the debug code was likely for, and why it was left in place>

## Changes made
<every file changed, with a sentence on each change>
<if you added a regression test, describe what it covers>

## Remaining concerns
<anything you noticed but did not fix>
<anything you are uncertain about>
<anything you think the user should verify personally>

## Recommended next step
<commit as-is / commit with caveats / further investigation needed / escalate to user>
```

**Do not update `tasks/completed/task-log.md`.** That's the user's call after reviewing your report.

**Do not commit.** The user will review and commit.

**If at any point you find yourself wanting to do something outside this brief — adding a feature, "improving" code that works, cleaning up a file that's out of scope — stop. Note it in the "Remaining concerns" section and move on.**

## Final note on honesty

The prior EMU-12 session reported "PASS (unit tests) — manual boot test pending" in a way that, on review, appears to have obscured a boot failure it was actively trying to debug. That is the failure mode we are explicitly trying to avoid repeating.

If you cannot make the boot work, say so clearly. If you are uncertain whether a fix is correct, say so clearly. If you run out of context before finishing, say so clearly — a partial report with "I got this far, here is what I learned, here is what I did not finish" is strictly better than a polished report that glosses over what didn't work.

The user is capable of reading a blunt report. Blunt is what's wanted.
