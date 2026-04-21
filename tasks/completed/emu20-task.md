# EMU-20: Align shift AF behaviour with reference (preserve AF instead of clearing)

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Where we are

The differential test harness (EMU-16/17/19) runs our emulator in lockstep with the reference `8086tiny` and halts on any divergence. EMU-19 closed the step-4096 timer cadence bug; the harness now reaches step 65,679 before reporting a new divergence.

### The divergence, correctly attributed

The divergence report from step 65,679 says `CS:IP = F000:10FA`, opcode bytes `88 EB ...`, with only AF differing (ref=1, ours=0). **This is misleading**. The harness reports state *after* the step executes, so the CS:IP shown is where the next instruction *starts*, not where the divergent instruction was. The actual divergent instruction was at `F000:10F8` — two bytes back:

```
F000:10F8  D2 EF   SHR BH, CL
```

The run was halted cleanly at step 65,678 with `HARNESS_STEP_LIMIT=65678`; CS:IP at that point was `F000:10F8`, confirming step 65,679 begins there.

### The root cause

**Intel leaves AF undefined after shift instructions.** From Intel's 8086 documentation for SHL, SHR, SAR: "The AF flag is undefined." This means different implementations may legitimately produce different AF values. Neither is "wrong" per Intel — but the harness compares AF bit-for-bit, so implementation choices that differ show up as divergences.

The reference (`reference/8086tiny.c`) does not update AF during shift operations. Specifically:

- `std_flags[0xD2]` = 0, meaning opcode 0xD2's post-dispatch flag block doesn't run.
- Inside the shift dispatch (case 12), `set_opcode(0x10)` re-maps to opcode 0x10, whose `std_flags` entry is `1` (SZP only, no AO_ARITH).
- `set_AF_OF_arith()` is therefore never called.
- Consequently, AF simply carries its value forward unchanged across the shift.

Our emulator (`packages/emu86/src/emulator/opcodes/shift.h`) does the opposite: it **explicitly clears AF** at the end of SHL, SHR, and SAR. Three lines of `clear_flag(s, FLAG_AF)` — one in each function. Our rotates (ROL, ROR, RCL, RCR) correctly do not touch AF, matching the reference.

Both behaviours are defensible per Intel. But since our validation methodology is "match the reference," we match: remove the three AF-clearing lines.

## Your task

### Phase 1 — Verify the state

Check that the current tree reproduces the step-65,679 divergence as described:

```
cd packages/emu86
make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected: divergence reported at step 65,679, CS:IP `F000:10FA`, only `FLAGS` in categories, AF differing. If it doesn't reproduce as described, stop and report.

Also confirm the pre-step state:

```
HARNESS_STEP_LIMIT=65678 ./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -5
```

Expected: "reached step limit 65678 with no divergence. Final CS:IP=F000:10F8" — confirming the divergence-causing instruction is at `F000:10F8`.

### Phase 2 — Apply the fix

Remove `clear_flag(s, FLAG_AF);` from three functions in `packages/emu86/src/emulator/opcodes/shift.h`:

- `exec_shl` — the line around line 34
- `exec_shr` — the line around line 62
- `exec_sar` — the line around line 104

That's it. Three deletions. Do not refactor surrounding code. Do not add replacement logic. Do not touch rotate functions (ROL/ROR/RCL/RCR) — those already don't touch AF and match the reference.

If you find yourself wanting to change more than those three lines, stop and explain why.

### Phase 3 — Regression tests

Add unit tests to `packages/emu86/test/unit/test_shift.c` (or wherever the shift opcode tests live) that verify:

- **AF is preserved across SHL.** Set AF=1 (via an arithmetic op that sets it, or directly if the test harness permits), run an SHL, verify AF is still 1 afterwards. Run an SHL starting with AF=0, verify AF remains 0.
- **AF is preserved across SHR.** Same pattern.
- **AF is preserved across SAR.** Same pattern.
- **AF is still not touched by rotates.** Set AF=1, run ROL/ROR/RCL/RCR, verify AF=1 afterwards. This is a regression guard for the functions you're *not* changing — you don't want a future refactor accidentally clearing AF in the rotates.

Construct the tests to both fail against the pre-fix code (where SHL/SHR/SAR clear AF) and pass against the post-fix code. Do the revert-and-re-test discipline: confirm the new SHL/SHR/SAR tests fail with the fix reverted; re-apply the fix; confirm they pass.

Rotate tests should pass both before and after the fix (they're regression guards, not failure-before tests).

Run the full test suite after the fix and confirm no existing test regresses.

### Phase 4 — Verify via harness

Re-run the full harness:

```
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Three possible outcomes:

**(a) Step-65,679 divergence is gone; harness runs further before finding a new divergence.** Record the new divergence (instruction count, opcode, diverging state) in the Follow-up section of the commit. Do not fix it.

**(b) Step-65,679 divergence is gone; harness runs to the 200M-step limit with no further divergence.** Excellent — record the step count reached.

**(c) Divergence still reported at step 65,679 or earlier.** The fix didn't work, or the analysis was wrong. Stop and report; do not commit.

### Phase 5 — Standalone check

Confirm standalone `./emu86` still reaches the FreeDOS kernel banner:

```
./emu86 reference/bios test/images/freedos.img
```

Expected: same kernel banner and divide-by-zero loop as before. If behaviour regresses, stop and report.

### Phase 6 — Update the quirks documentation

Append a new entry to `docs/notes/8086tiny-quirks.md` documenting this alignment. Template:

```markdown
## AF flag after shift instructions (Intel-undefined)

**Location:** `reference/8086tiny.c` shift dispatch (case 12) and our
`packages/emu86/src/emulator/opcodes/shift.h`.

**Behaviour:** Intel's 8086 specification explicitly says "The AF flag
is undefined" after SHL, SHR, and SAR. Different implementations may
produce different AF values without being wrong per the spec. The
reference emulator preserves AF across shift instructions (never
calls set_AF, leaves the existing value untouched). Our emulator
previously cleared AF to 0 at the end of these operations.

**Design rationale:** Both are defensible per Intel. We align with the
reference to keep the differential harness clean. Per our
methodology (see `methodology.md`), matching the oracle is the
correctness criterion.

**What we do:** EMU-20 removed the `clear_flag(s, FLAG_AF)` calls
from `exec_shl`, `exec_shr`, and `exec_sar`. AF now carries its
previous value across shifts, matching the reference.

**Identified during:** EMU-20 investigation (following a harness
divergence at step 65,679 during FreeDOS boot).

**Historical note:** Similar behaviour for rotate instructions
(ROL, ROR, RCL, RCR) was already correct — none of these touched AF
in either emulator. Our regression tests now explicitly guard
against regression in that area as well.
```

Adjust the wording if it reads awkwardly in context.

### Phase 7 — Commit

**Commit** if: Phase 1 reproduced the bug, Phase 2 applied the three-line fix, Phase 3's regression tests fail-before and pass-after (for the three shifts; rotate tests pass always), Phase 4 produced outcome (a) or (b), Phase 5 confirmed standalone behaviour unchanged, and Phase 6 updated the quirks doc.

Commit message:

```
EMU-20: Align shift AF behaviour with reference

Bug: Our SHL/SHR/SAR explicitly cleared AF; reference preserves AF
     across shifts. Intel spec says AF is undefined after these ops,
     so both are defensible — we align with the reference for
     harness lockstep.

Fix: Removed `clear_flag(s, FLAG_AF)` from exec_shl, exec_shr,
     exec_sar in opcodes/shift.h. Three deletions.

Tests: N new tests in test_shift.c verifying AF preserved across
       SHL/SHR/SAR (fail-before, pass-after confirmed by revert-
       and-re-test). Plus rotate regression guards (ROL/ROR/RCL/
       RCR) confirming they also don't touch AF — these pass both
       before and after the fix.

Harness result: [outcome (a) or (b) details]

Standalone: unchanged — FreeDOS kernel banner reached, divide-by-
            zero loop as before.

Docs: Added `AF flag after shift instructions` entry to
      docs/notes/8086tiny-quirks.md.

Follow-up: [next divergence if any]
```

Task log entry:

```
## EMU-20
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests for shift/rotate AF
Harness result: {step count reached, new divergence (if any)}
Notes: Aligned SHL/SHR/SAR with reference by removing AF-clearing.
Intel-undefined behaviour; matching the oracle for harness lockstep.
Quirks doc updated. Standalone ./emu86 unchanged.
```

Then:

```bash
mv tasks/emu20-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

**Report** to `tasks/triage/emu20-triage-report.md` if: Phase 1 doesn't reproduce, Phase 2 requires more than three line deletions, Phase 3 tests can't be made to fail-before and pass-after, Phase 4 produces outcome (c), or Phase 5 shows standalone regression.

## Out of scope — do not touch

- **Any new divergence the harness finds after the fix.** That's the next task.
- **Other shift/rotate quirks** including the 0xC0/0xC1 rotate-form latent bug flagged in `8086tiny-quirks.md`. Still deferred.
- **The FreeDOS divide-by-zero.** Separate bug, still pending.
- **Harness improvements** (better divergence reports, suppressing Intel-undefined comparisons, etc.). Follow-up work.
- Previous out-of-scope items: `editor-api-proposal.md`, latent 0xEA JMP/CALL length bug, silent-exit-on-0:0, register-memory aliasing, timer design questions.

## Housekeeping

- `/tmp/emu86-harness/` scratch files per EMU-17. Leave them.
- `packages/emu86/emu86-dbg` may still be present from EMU-14. Leave it.
- No new binaries should end up in the commit. Verify `git status` before `git add`.

## Final note

This is another "narrow fix, high confidence" task — the third in a row (EMU-15, EMU-19, EMU-20). The risk is the same: complacency. The deletion is trivial; the tests that prove the deletion is correct are the actual work.

Pay particular attention to the test construction. AF preservation across shifts is a subtle property — "AF is whatever it was before" is harder to test cleanly than "AF is 0" or "AF is 1." Set AF deliberately to a known non-default value, execute the shift, confirm AF is unchanged. Do this for both AF=0 and AF=1 to rule out accidental coincidence.

The rotate regression tests are specifically included because if a future refactor accidentally added `clear_flag(FLAG_AF)` to the rotate functions, no existing test would catch it (rotates have always been correct, so no one wrote tests for it). We add those tests now while the analysis is fresh and AF-preservation is the topic.

If anything in Phase 1–4 produces unexpected results, stop and report. Don't improvise.
