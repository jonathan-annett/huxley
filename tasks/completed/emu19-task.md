# EMU-19: Fix timer tick cadence to match reference

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Where we are

- EMU-16 built a differential test harness that runs our emulator in lockstep with the reference `8086tiny` (see `packages/emu86/harness/`). On its first real run it detected a divergence at instruction 4096: our emulator sets `int8_asap = 1` (ready to fire BIOS timer interrupt INT 8), the reference does not.
- EMU-17 added scratch-file disk isolation to the harness and reproduced the step-4096 divergence.
- EMU-18 investigated the divergence, identified the cause, and was properly committed as "premise falsified" (the originally-proposed virtual-clock design solved the wrong problem). See `tasks/triage/emu18-triage-report.md` for full evidence.
- `docs/notes/methodology.md` and `docs/notes/8086tiny-quirks.md` document the validation strategy and the specific timer-cadence behaviour this task matches.

### The bug

Our emulator in `packages/emu86/src/emulator/run.c` around line 587 has:

```c
if ((s->inst_count & 0x4FFF) == 0) {
    /* fire timer tick */
}
```

This fires at instruction counts 0x1000, 0x2000, 0x3000, 0x4000, 0x8000... — a bitmask pattern, not a regular cadence.

The reference fires the timer via `(inst_counter % KEYBOARD_TIMER_UPDATE_DELAY) == 0` where `KEYBOARD_TIMER_UPDATE_DELAY = 20000` (see `reference/8086tiny.c:710-711`). That's every 20,000 instructions exactly.

The bitmask in our code appears to be a mistranslation of the reference's modulo — possibly someone saw `% 20000`, noted that `0x4E20 = 20000`, and wrote `& 0x4FFF` instead. `&` and `%` do not compute the same thing, and `0x4FFF` is not `0x4E20`.

### The fix (prescribed)

Change `(s->inst_count & 0x4FFF) == 0` to `(s->inst_count % 20000) == 0` in `packages/emu86/src/emulator/run.c`.

One line. Same operator shape, different arithmetic.

### Why this is correct

Matching the reference's cadence is the correctness criterion for harness lockstep. `% 20000` is deterministic on both sides, produces a tick rate that approximates 18.2 Hz at original-IBM-PC speeds (good enough for the workloads 8086tiny was designed for), and is already documented as a deliberate design choice we inherit — see `docs/notes/8086tiny-quirks.md` under "Timer tick cadence."

This task is *not* the place to revisit whether instruction-count-based timing is the right design long-term. It isn't, in some hypothetical future where we want wall-clock-accurate timer behaviour. But it is right for matching the reference in the harness, which is what we need right now.

## Required reading before you touch code

1. **`tasks/triage/emu18-triage-report.md`** — full evidence of the bug and why the one-line fix is correct. The agent who wrote it did careful work; you should understand it before repeating or re-investigating.
2. **`docs/notes/8086tiny-quirks.md`**, section "Timer tick cadence" — the design-level context for why `% 20000` is what we want.
3. **`packages/emu86/src/emulator/run.c` around line 587** — the buggy code.
4. **`packages/emu86/reference/8086tiny.c` around line 710** — the reference's cadence code, for comparison.

## Your task

### Phase 1 — Verify the current state

Confirm the bug still reproduces before fixing it. Tree should be at master with EMU-18's closure commit as HEAD. Run the harness:

```
cd packages/emu86
make harness
./harness/harness reference/bios test/images/freedos.img
```

Expected output includes a divergence report at step 4096 with `int8_asap` differing between the two emulators (ref=0, ours=1 — or the opposite, depending on exact initial state).

If the divergence does not reproduce at step 4096, stop and report — something has changed since EMU-18 that needs investigation before this fix is applied.

### Phase 2 — Apply the fix

One-line change in `packages/emu86/src/emulator/run.c`, around line 587:

```c
// before
if ((s->inst_count & 0x4FFF) == 0) {

// after
if ((s->inst_count % 20000) == 0) {
```

Keep the change minimal. Do not refactor surrounding code. Do not "improve" the comment if one exists, unless it currently contains something factually wrong about the cadence.

If you find yourself wanting to change more than one line, stop and explain why a single-line fix doesn't work.

### Phase 3 — Regression tests

Add unit tests to `packages/emu86/test/unit/test_run.c` that verify the timer cadence. Concrete tests to include:

- **Cadence correctness.** Run the emulator for `N` instructions where `N > 20000`. Count how many times `int8_asap` transitions from 0 to 1 (the rising edge). For `N = 20000 * k`, the count should be exactly `k`. For `N = 20000 * k + r` where `0 < r < 20000`, the count should still be `k` (the next edge wouldn't have fired yet).

- **First tick location.** The first rising edge of `int8_asap` should occur at instruction count 20000, not at 4096.

- **Cadence regularity.** The intervals between consecutive rising edges should all equal 20000.

You can construct these tests by either (a) using the existing unit-test harness to step the emulator in a loop with a simple no-op-like instruction and sampling `int8_asap`, or (b) adding a small helper that drives the emulator through N instructions and returns a count of rising edges. Either approach is fine; pick whichever fits the existing test patterns better.

**Revert-and-re-test discipline.** Before declaring the tests adequate, confirm they *fail* against the unpatched code. Apply the fix only temporarily-revertible for this check: run tests with the fix → all green; revert the fix → run tests → all three above should fail; re-apply the fix. Mention in the commit message that you performed this check.

Run the full test suite after the fix and confirm everything still passes.

### Phase 4 — Verify the harness result

Re-run the harness with the fix in place:

```
./harness/harness reference/bios test/images/freedos.img
```

Three possible outcomes:

**(a) The step-4096 divergence is gone, and the harness runs further before finding a new divergence (or completes with no divergence).** Best case. Record the new divergence (if any) in your commit message's Follow-up section; do not fix it.

**(b) The step-4096 divergence is gone, and the harness runs to some large step count with no divergence.** Also good. Record the step count reached.

**(c) A divergence still fires at step 4096.** The fix didn't solve the problem, or there's something else happening at 4096 besides the timer. Do not commit — stop and report.

**(d) A divergence fires earlier than step 4096.** Unexpected. Stop and report.

### Phase 5 — Standalone regression check

Confirm standalone `./emu86` still works for the cold-boot workload:

```
./emu86 reference/bios test/images/freedos.img
```

Expected: reaches the FreeDOS kernel banner and the familiar divide-by-zero loop, same as before EMU-19. The fix should not regress standalone behaviour.

If it does regress, stop and report — something in your change has affected more than just the cadence.

### Phase 6 — Commit (on clean success) or report (on failure)

**Commit** if: Phase 1 reproduced the bug, Phase 2 applied the one-line fix, Phase 3's regression tests fail-before and pass-after, Phase 4 produced outcome (a) or (b), and Phase 5 confirmed standalone behaviour is unchanged.

Commit message structure:

```
EMU-19: Fix timer tick cadence (run.c:587)

Bug: [one sentence on what was wrong]

Fix: [the one-line change, quoted; arithmetic showing old and new
      cadences]

Tests: [N new regression tests in test_run.c covering cadence correctness,
        first-tick location, and interval regularity. Revert-and-re-test
        confirmed they fail against unpatched code.]

Harness result: [outcome (a) or (b), with step count reached and any
                 new divergence summarised]

Standalone check: [confirmed unchanged]

Follow-up: [any new harness divergence becomes the next task; anything
            else noticed during the work]
```

Task log entry appended to `tasks/completed/task-log.md`:

```
## EMU-19
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests for timer cadence
Harness result: {step count reached; new divergence at step M (if any)}
Notes: Fixed timer tick cadence in run.c:587 to match reference's
% 20000. Closed the step-4096 divergence identified by EMU-16 and
diagnosed by EMU-18. {Summary of harness result.} Standalone
./emu86 boot unchanged.
```

Then:

```bash
mv tasks/emu19-task.md tasks/completed/
git add -A
git commit
```

Do **not** push. User reviews and pushes manually.

**Report** to `tasks/triage/emu19-triage-report.md` if: Phase 1 doesn't reproduce the bug, Phase 2 requires more than one line, Phase 3 can't produce tests that fail-before and pass-after, Phase 4 produces outcome (c) or (d), or Phase 5 shows standalone regression.

Report structure same as prior triage reports.

## Out of scope — do not touch

- **Any new divergence the harness finds after the fix.** That's the next task, not this one.
- **The wall-clock-timing question.** Whether the emulator should eventually drive the timer from wall-clock is a future design question, not a bug fix. Don't start down that path here.
- **The FreeDOS divide-by-zero that follows.** Still a separate bug; still the next thing after whatever this task enables the harness to find.
- **Harness improvements.** If the harness's divergence report format could be better, that's follow-up work.
- **Snapshot format changes.** This fix shouldn't require any.
- `editor-api-proposal.md`, latent 0xEA JMP/CALL length bug, silent-exit-on-0:0, register-memory aliasing, 0xC0/0xC1 rotate-form reference bug. All previous out-of-scope items, still out of scope.

If you notice anything worth raising, put it in the Follow-up section of the commit or the Remaining-concerns section of the report.

## Housekeeping

- Scratch files live in `/tmp/emu86-harness/` per EMU-17. Don't change this.
- `packages/emu86/emu86-dbg` may still be present from EMU-14. Leave it.
- No new untracked binaries should end up in the commit. Verify `git status` before `git add`.

## Final note

This is the narrowest bug-fix task we've had in the project to date. The fix is prescribed, the tests are prescribed, the acceptance criteria are concrete. The discipline this task exists to exercise is *not* design judgement — it's execution quality. Write the tests so they actually fail against broken code. Confirm the standalone path still works. Read the harness output carefully before declaring success.

The failure mode for this task is the same one EMU-15 had: *it's too easy, so it doesn't get the care it deserves.* The one-character fix takes thirty seconds. The regression tests that prove the fix is correct take longer and are where the actual value lives. Don't cut corners on them.

If you get unexpected results at any phase — anything that doesn't match the brief's prediction — stop and report. Don't improvise.
