# EMU-23: Resolve exec_lea dead code in opcodes/transfer.h

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Required reading

**`tasks/triage/emu21-triage-report.md`** — specifically the section "Finding: `exec_lea` in `transfer.h` is dead code on the runtime path" and the related entry in "Follow-up concerns." The previous agent discovered and documented this issue; read their findings before deciding.

Also:

- **`packages/emu86/src/emulator/opcodes/transfer.h`**, function `exec_lea` — the dead code in question.
- **`packages/emu86/src/emulator/run.c`**, case 10 (opcode 0x8D, LEA) — the runtime implementation that's actually called.
- **`packages/emu86/test/unit/test_transfer.c`** — the two unit tests that call `exec_lea` directly: `lea_basic` and `lea_no_memory_access`.

### The situation, briefly

LEA (opcode 0x8D) has two implementations in our codebase:

1. **Runtime path — in `run.c` case 10, inlined.** This is what executes during `emu86_run`. Uses the decode-table-derived default segment (correctly picks SS for `[BP]`-based addressing, DS for `[BX]`, etc.).

2. **Unit-test-only path — `exec_lea` in `opcodes/transfer.h`.** Not reachable from the runtime. Used by two unit tests. Has a latent bug: when `seg_override_en=0`, it hardcodes `SREG_DS` as the default segment (line 247 per the triage report). This is wrong for `[BP]`-based addressing. The latent bug is masked in the existing tests because both tests set `SREG_DS = 0`.

This is a DRY violation: two implementations of the same instruction, with the dead one subtly wrong. EMU-23 resolves it.

### What this task is *not*

- Not a harness advance. This is hygiene. After EMU-23 commits, the harness will still diverge at step 66,392 (the ADD/ZF divergence left over from EMU-22). That's EMU-24's problem.
- Not a behavioural change. Whichever path you choose (delete or wire in), the runtime LEA's behaviour must be unchanged. Standalone `./emu86` and the harness must run identically before and after this task.
- Not an opportunity to rework LEA. Leave the runtime inlined implementation alone unless you're wiring `exec_lea` in as its replacement (see below).

## Your task

### Phase 1 — Confirm the situation

Verify the dead-code claim. Search the tree for any caller of `exec_lea` other than the two test files:

```
cd packages/emu86
grep -rn "exec_lea" --include="*.c" --include="*.h"
```

Expected: callers limited to `test/unit/test_transfer.c` (the two tests named in the triage report) and possibly a forward declaration in `opcodes/transfer.h` itself. If there's any runtime caller not named in the triage report, stop and report — the triage report may have missed something.

Also confirm the runtime LEA is inlined in `run.c` case 10 (lines ~220-232), matching the triage report's description.

### Phase 2 — Decide: delete or wire in

Two defensible paths. Both land a clean commit.

**Option A: Delete.**

Remove `exec_lea` from `opcodes/transfer.h`. Remove the two tests from `test/unit/test_transfer.c`. Runtime behaviour is unchanged — runtime LEA was never calling `exec_lea`. Test coverage for LEA is retained via EMU-22's three new runtime-path LEA tests in `test_run.c`.

Pros: smallest change, least code, cleanest. The dead function and its dead-bug-masking tests both disappear together.

Cons: the two old tests named `lea_basic` and `lea_no_memory_access` are gone. If they tested edge cases the new runtime-path tests don't cover, those cases become untested.

**Option B: Wire in.**

Fix `exec_lea`'s hardcoded-DS bug so it matches the runtime's table-derived default segment. Refactor `run.c` case 10 to call `exec_lea` instead of inlining. Update or keep the two existing tests.

Pros: DRY properly — one implementation, one place. If future LEA work is needed, there's one spot to change.

Cons: larger change. Requires moving the segment-table logic out of `run.c` and into `transfer.h`, or passing the computed segment into `exec_lea` as a parameter. The exact shape depends on what `exec_lea` currently takes and what information it needs.

**Choose based on examination.** Before picking, look at the runtime inlined code carefully. If the inlined logic is short and localised (2-5 lines), Option A (delete) is probably right — a function wrapper for 5 lines isn't buying anything. If the runtime logic is longer or has meaningful decomposition potential, Option B (wire in) has more value.

My weak preference is Option A. The runtime path works, its tests from EMU-22 cover the cases that matter, and removing dead code is unambiguous hygiene. But the call is yours after reading the code.

Document your choice in the commit message with one-sentence rationale ("chose delete because runtime inlined implementation is 5 lines" or "chose wire-in because the shared logic is substantial enough to warrant the function").

### Phase 3 — Implement

#### If Option A (delete):

1. Remove `exec_lea` from `opcodes/transfer.h`. Also remove any forward declarations or includes that only supported it.

2. Remove the two tests from `test/unit/test_transfer.c`:
   - `lea_basic`
   - `lea_no_memory_access`
   Also remove any test-file-local helpers that supported only those tests.

3. If any test registration/enumeration mechanism needs updating (e.g., a test list in `test_transfer.c` or a test-runner config), update accordingly.

4. Build and verify full test suite passes.

#### If Option B (wire in):

1. Fix `exec_lea`'s default-segment logic to match the runtime's table-derived approach. The runtime uses `read_table_sreg(s, seg_reg_idx)` where `seg_reg_idx = t->data[tbase + 3][d->rm]`. `exec_lea` needs access to this information — either by computing it itself (duplicating the table lookup), or by accepting the segment as a parameter.

2. Modify `run.c` case 10 LEA branch to call `exec_lea` instead of inlining. Pass whatever parameters the refactored `exec_lea` requires. Retain the `s->seg_override_en = 1` side effect from EMU-22 — either inside `exec_lea` or at the call site.

3. Update or keep the existing `lea_basic` and `lea_no_memory_access` tests. If they need adjustment to exercise the new signature, adjust them. Ensure they exercise the `[BP]`-based case that was previously masked — this is the whole point of fixing the latent bug.

4. Add a regression test: LEA with `[BP+disp]` addressing, `SREG_DS != 0`, verify the offset is computed without DS contribution (i.e., against SS as the implicit default). This test should have failed against the pre-fix `exec_lea`.

5. Build and verify full test suite passes.

### Phase 4 — Verify nothing else regressed

Regardless of option chosen:

```
cd packages/emu86
make clean && make
make test         # or whatever the full test command is
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -10
./emu86 reference/bios test/images/freedos.img    # (run briefly, confirm FreeDOS banner)
```

Expected:
- All unit tests pass. Count should be what EMU-22 reported (1563) minus 2 if Option A (the two removed tests), or 1563 + N if Option B (any new regression tests added).
- Harness reaches step 66,392 and diverges there on FLAGS/ZF at ADD SI,AX (exactly the state EMU-22 left us in). If it diverges earlier or later, something regressed — stop and report.
- Standalone reaches the FreeDOS banner and the familiar divide-by-zero loop.

### Phase 5 — Commit

Commit message:

```
EMU-23: Remove (or refactor) dead exec_lea in opcodes/transfer.h

Context: EMU-21's triage discovered that exec_lea in transfer.h is
         dead code on the runtime path — runtime LEA is inlined
         directly in run.c case 10. The dead function additionally
         has a latent hardcoded-DS default-segment bug, masked by
         the two unit tests that both set SREG_DS=0.

Approach: [Option A / Option B] chosen because [one-sentence rationale].

Changes:
- [deleted / refactored] exec_lea in opcodes/transfer.h
- [deleted / updated] lea_basic and lea_no_memory_access tests in
  test_transfer.c
- [if Option B: relevant run.c change; new regression test covering
  [BP+disp] with SREG_DS!=0]

Tests: {X passed, Y failed} before and after.

Harness: unchanged — still diverges at step 66,392 on ADD SI,AX
         ZF mismatch (EMU-22's follow-up).

Standalone: unchanged — FreeDOS banner reached.

Follow-up: EMU-24 to investigate the step-66,392 ZF divergence.
```

Task log entry:

```
## EMU-23
Date: {today}
Status: PASS
Test results: {X passed, Y failed}
Harness result: unchanged (step 66,392 divergence preserved)
Notes: Resolved exec_lea dead code per EMU-21 triage finding.
Chose [delete / wire-in]. [Brief rationale.] Runtime LEA path
unchanged. Standalone and harness behaviour preserved.
```

Then:

```bash
mv tasks/emu23-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

### Phase 6 — Report (on failure)

Report to `tasks/triage/emu23-triage-report.md` if: Phase 1 reveals unexpected runtime callers, Phase 2's analysis suggests neither option is clean (e.g., `exec_lea` has complex dependencies that make it hard to delete and equally hard to wire in), Phase 3's implementation hits structural obstacles, or Phase 4 shows any regression.

## Out of scope — do not touch

- **The step-66,392 ZF divergence.** EMU-24.
- **Any other dead code.** Focus on `exec_lea` specifically.
- **Any DRY cleanup elsewhere.** Same.
- **Harness changes.** Leave it alone.
- **Snapshot or state-struct changes.**
- Previous out-of-scope items: editor-api-proposal, latent 0xEA length bug, 0xC0/0xC1 rotate-form reference bug, silent-exit-on-0:0, register-memory aliasing, timer design questions, FreeDOS divide-by-zero.

## Housekeeping

- Scratch files in `/tmp/emu86-harness/` per EMU-17. Leave them.
- `packages/emu86/emu86-dbg` may still be present. Leave it.
- Harness instrumentation (opcode bytes at step-limit exit) is committed and shouldn't need attention.
- No new binaries in the commit.
- Makefile quirk: force rebuild with `touch src/emulator/run.c && make harness` if builds seem stale.

## Final note

This is hygiene, not forward progress. The reward is a cleaner codebase, not a harness advance. Resist the urge to fix adjacent things you notice along the way — each unrelated improvement is a separate task. The discipline of "one narrow change per commit" is how the repo stays reviewable; broadening this task to "clean up transfer.h generally" would undermine that.

Specifically: if you notice other functions in `transfer.h` that look questionable, or test files that have similar issues, or comments that have gone stale, make a note for yourself but do not touch them in this commit. They become their own tasks if warranted.

Read the existing code carefully before choosing between Option A and Option B. The choice should be obvious once you've seen the shapes involved. If it isn't obvious, default to Option A (delete) — dead code is always safe to remove, and simplicity is a feature.
