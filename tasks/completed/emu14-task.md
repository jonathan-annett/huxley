# EMU-14: Resolve boot failure at instruction ~156,246

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` including `emu12-task.md`, `emu13-task.md`, and a triage report at `tasks/triage/emu12-triage-report.md`. Those are *reference material*, not your assignment. Phase 1 of this brief asks you to read specific parts of them. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. The project uses a task-based workflow: one task per commit, task files in `tasks/`, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

EMU-12 committed the Linux host platform (PARTIAL — boot blocked). EMU-13 fixed the first of two identified bugs (ALU-immediate instruction length for opcodes 0x80–0x83) and is now committed and pushed. With that fix in place, FreeDOS execution advances from 209 instructions to ~156,246 instructions before exiting silently at CS:IP=0:0. It still doesn't reach the `A:\>` prompt.

Your task is to find out why, and either fix it cleanly or explain precisely why the fix belongs in a separate task.

## The working hypothesis (to verify, not assume)

The EMU-12 triage report flagged, as a latent concern, that the original 8086tiny aliases CPU registers into memory at `0xF0000` via `regs8 = mem + REGS_BASE`. Our refactor keeps registers in `Emu86State.regs[]`, separate from `mem[]`. If the BIOS reads or writes bytes in the range `0xF0000–0xF00FF` expecting those to be CPU registers (or writes to register-file addresses expecting those writes to be visible as memory), we'll produce wrong values.

The triage report predicted this "may surface once Bug 2 is fixed." The jump in execution depth (209 → 156,246 instructions) is consistent with that — Bug 2 was blocking most BIOS execution, and fixing it has exposed a deeper latent issue. But "consistent with" is not "confirmed as." **Your job is to verify whether this hypothesis is actually correct before acting on it.**

It is possible:

- The hypothesis is correct and the fix is small (initialisation aliasing, or a memory-access redirect).
- The hypothesis is correct but the fix is architectural (affects snapshot format, opcode helpers, platform interface — many files).
- The hypothesis is wrong and the real cause is something else entirely.

Each case has a different response. Don't collapse the three into one.

## Required reading before you touch code

1. **`tasks/triage/emu12-triage-report.md`**, specifically the "Remaining concerns" section. Pay close attention to the "Register-memory mapping gap" paragraph — it is the basis of the hypothesis.
2. **`packages/emu86/docs/ORIGINAL-ANALYSIS.md`**. Look for how the original 8086tiny handles the register file. Note the `regs8`, `regs16`, and `REGS_BASE` definitions (or equivalent). Understand what memory addresses alias to which registers.
3. **`packages/emu86/src/emulator/`**, particularly wherever CPU register access is implemented (`helpers.h`, `run.c`, opcode files). Understand how our refactor separates `regs[]` from `mem[]`.
4. **`packages/emu86/reference/8086tiny.c`**. Briefly — just enough to confirm the aliasing model.
5. Look at `packages/emu86/reference/bios.asm` or its disassembly if available. The failure happens somewhere around instruction 156,246; you will need to correlate that with what the BIOS is doing.

## Your task

### Phase 1 — Reproduce and characterise precisely

Build and run, confirm FreeDOS still fails as described:

```
cd packages/emu86
make
./emu86 reference/bios test/images/freedos.img
```

Reproduce the silent exit. Then determine, with evidence:

- **Where does execution stop?** Exact `CS:IP` at the moment of failure. The prior reports said "CS:IP=0:0" — is that literally zero, or is it a near-zero address? What was the CS:IP *before* the final transition?
- **What was the last instruction executed before the bad transition?** What was in memory at that address? What was the decoded opcode?
- **What was the CPU state at that point?** Registers, flags, stack pointer.
- **Is this a deterministic stopping point?** Runs with identical inputs should fail at the same instruction count. Confirm this.
- **Does any console output appear before the failure?** The emulator has a console ring buffer; check whether anything reached it.

You may add *minimal, targeted* diagnostic output to establish these facts — for example, printing CS:IP and the opcode byte when `inst_count` is near 156,000. Any such diagnostic code must be removed before committing. Do not repeat the prior EMU-12 agent's mistake of leaving unconditional `fprintf` blocks in the run loop.

If the reference emulator is a useful oracle — booting the same image in `reference/8086tiny` with instrumentation of its own — that's a legitimate approach. It establishes ground truth without having to reason from first principles.

### Phase 2 — Verify or falsify the hypothesis

The hypothesis is: the failure is caused by a mismatch between our separated `regs[]`/`mem[]` and the original's aliased register-in-memory model, triggering when the BIOS reads or writes in the `0xF0000` range.

To verify, you need concrete evidence of at least one of:

- A load from an address in `0xF0000–0xF00FF` returning a value that doesn't match what would be there if the register file were aliased.
- A store to such an address not being reflected in subsequent register reads.
- An instruction that dereferences a computed address in that range, where the expected semantics depend on aliasing.

A handwave like "the BIOS accesses `0xF0000`, so this must be the bug" is not verification. You need to show that specific accesses are producing wrong values and that those wrong values are what causes the eventual silent exit.

**If the evidence confirms the hypothesis**, proceed to Phase 3.

**If the evidence contradicts the hypothesis** (for example, the failure is caused by a different bug — a missing opcode case, a flag calculation error, an off-by-one somewhere), characterise the actual cause precisely, skip to Phase 5, and report.

**If the evidence is ambiguous**, report what you found, do not guess. Ambiguity is a valid outcome worth reporting.

### Phase 3 — Assess fix scope

Assuming Phase 2 confirms the hypothesis: is the fix localised or architectural?

A **localised** fix is something like: during `Emu86State` initialisation, set up `regs[]` to be a view into `mem[0xF0000]` (using a pointer or union or equivalent), so that memory accesses in that range and register accesses share the same storage. If this can be done in a handful of lines in state setup and doesn't require changes to snapshot serialisation, opcode implementations, or the platform interface, it is localised.

An **architectural** fix is something like: changing how `Emu86State` stores registers, changing the snapshot format, adding address-range checks to every memory read and write, modifying multiple opcode families to use a new helper. These are scope violations for EMU-14.

Judge honestly. "I can make it work with a small change" is fine. "I can make it work but it will need careful handling in five other places" is architectural. If it's architectural, **do not attempt it here** — stop, write the report, and let it become its own task.

### Phase 4 — Apply fix and verify (only if Phase 3 says localised)

Apply the fix. Add targeted regression tests to `packages/emu86/test/unit/` — ideally in a new file if the existing ones don't have a natural home for register-memory alias tests. The tests should:

1. Confirm that a write to `mem[0xF0000]` is visible as the corresponding register read.
2. Confirm that a write to a register is visible as a read from `mem[0xF0000 + offset]`.
3. Cover at least the word-sized and byte-sized access patterns (both high and low bytes of a register).
4. Include at least one test that would fail without the fix — run it against unpatched code to confirm, then re-patch. This is the same revert-and-re-test discipline EMU-13 used.

Then run the full test suite and confirm everything still passes. The fix should not break any existing test.

Finally, attempt the boot:

```
./emu86 reference/bios test/images/freedos.img
```

Three outcomes again:

**(a) FreeDOS reaches the `A:\>` prompt.** Milestone achieved. Note it in the commit.

**(b) FreeDOS fails differently than at 156,246.** New bug surfaced. Characterise the new failure briefly, note it as follow-up, do not fix it here.

**(c) FreeDOS fails the same way as before the fix.** The fix did not solve the problem — either the hypothesis was wrong (go back to Phase 2) or the fix is incomplete. Do not commit.

### Phase 5 — Commit (on clean success) or report (otherwise)

**Commit** if and only if: Phase 2 confirmed the hypothesis, Phase 3 judged the fix localised, Phase 4 applied the fix and produced outcome (a) or (b), regression tests pass, revert-and-re-test was performed, and all diagnostic scaffolding has been removed.

Commit message structure — same self-audit discipline as EMU-13:

```
EMU-14: {brief description}

Bug: {one paragraph on what was wrong and why}

Fix: {the change, with rationale for why it's correct}

Tests: {N new tests, what they cover, revert-and-re-test confirmation}

Boot test: {outcome (a) or (b), with specifics}

Follow-up: {anything surfaced that becomes the next task}
```

Task log entry appended to `tasks/completed/task-log.md`:

```
## EMU-14
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests for {description}
Boot test: {outcome}
Notes: {summary, including verified hypothesis, fix approach, and any follow-up concerns}
```

Then:

```bash
mv tasks/emu14-task.md tasks/completed/
git add -A
git commit  # paste the self-audit message
```

Do **not** push. The user will review and push manually.

**Report** to `tasks/triage/emu14-report.md` if: Phase 2 contradicted or couldn't confirm the hypothesis, Phase 3 determined the fix is architectural, Phase 4 produced outcome (c), or you encountered anything else preventing a clean commit.

Report structure:

```markdown
# EMU-14 Report
Date: {today}
Outcome: {HYPOTHESIS-FALSIFIED / ARCHITECTURAL / FIX-FAILED / BLOCKED}

## Summary
{2-3 sentences}

## Evidence
{what you found in Phase 1 and Phase 2}

## Root cause
{what's actually going on}

## Scope assessment
{why this isn't a localised fix, or what the real cause is if hypothesis was wrong}

## Changes made
{anything you modified — should be nothing if you're reporting rather than committing}

## Remaining concerns
{other things noticed, not fixed}

## Recommended next step
{what should happen in the next task or conversation}
```

**Do not commit if you're reporting.** Leave the tree clean, or if you made diagnostic changes to establish Phase 1/2 facts, revert them before declaring the session done.

## Out of scope — do not touch

The EMU-12 triage report listed other latent concerns beyond the reg-mem mapping:

- `i_mod_size` treated as boolean rather than multiplier
- Silent exit on `CS:IP = 0:0` with no output
- The decoder's linear length formula (the `CASE8` fix in EMU-13 was the specific bug; the broader "does the length formula have other edge cases" question is not for this task)

If you notice any of these or anything else during the work, note them in your commit's "Follow-up" section or your report's "Remaining concerns." **Do not fix them here.**

The `editor-api-proposal.md` in `packages/emu86/docs/` is an unratified proposal. It is not yours to ratify, modify, or act on.

## Final note on honesty

This task has a stronger prior than EMU-13 did — the triage report predicted exactly this scenario. That is both a gift and a trap. It's a gift because you start with a plausible hypothesis rather than a blank slate. It's a trap because the natural tendency under a strong prior is to look for confirming evidence and stop. The Phase 2 wording is deliberate: verify, don't assume, and be prepared to reject the hypothesis if the evidence doesn't actually support it.

Writing "I confirmed the hypothesis because the fix worked" is circular — a fix that incidentally improves things can look like a confirmation. What you want is evidence at Phase 2 that is independent of whether the fix works in Phase 4. If you can't produce that evidence without running the fix, say so in your report.

If at any point you're uncertain — about the cause, about whether your tests are adequate, about whether the fix is localised or architectural — stop and report. A partial deliverable with honest uncertainty is worth more than a confident-sounding commit that papers over what didn't get verified.
