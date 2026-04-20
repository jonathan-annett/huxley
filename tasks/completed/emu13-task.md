# EMU-13: Fix ALU-immediate instruction length (0x80–0x83)

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. The project uses a task-based workflow: one task per commit, task files in `tasks/`, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

EMU-12 (Linux host platform and CLI entry point) was committed PARTIAL — unit tests passed but FreeDOS does not boot. A subsequent triage (`tasks/triage/emu12-triage-report.md`) identified **two** bugs that were blocking boot. One (PUSH/POP sreg dispatch) was fixed during triage. The other was left out-of-scope for triage because it is an opcode-family bug rather than a dispatch wiring bug. **Fixing that second bug is your task.**

## Required reading before you touch code

1. **`tasks/triage/emu12-triage-report.md`** — the full investigation. Read the "Bug 2" and "Root cause" sections carefully. The report proposes a fix; your job is not to rubber-stamp it but to understand *why* it works.
2. **`packages/emu86/docs/ORIGINAL-ANALYSIS.md`** — the EMU-01 analysis of 8086tiny. Relevant sections: how the original handles opcode 0x80–0x83 (it calls `set_opcode(0x08 * (extra = i_reg))` to rewire the instruction length calculation) and what the decoder's length formula looks like (`base + modrm_disp + iw_size * (w+1)`, applied linearly).
3. **`packages/emu86/src/emulator/run.c`, case 8** — the buggy code. Study how `d->inst_length` is computed at decode time and then adjusted at execute time. The bug is in the interaction between those two steps, not in either one alone.

## The bug in plain terms

The decoder computes `inst_length` linearly using `iw_size * (operand_width + 1)` for the immediate. For opcodes 0x80–0x83 the BIOS tables give `iw_size = 1`, so the decoder always counts 2 bytes for the immediate when `operand_width = 1`, or 1 byte when `operand_width = 0`.

But the *real* immediate for this opcode family is:

| Opcode | operand_width (w) | direction (d) | sign_ext = d \| !w | Real immediate size |
|--------|-------------------|---------------|---------------------|---------------------|
| 0x80   | 0                 | 0             | 1                   | 1 byte              |
| 0x81   | 1                 | 0             | 0                   | 2 bytes             |
| 0x82   | 0                 | 1             | 1                   | 1 byte              |
| 0x83   | 1                 | 1             | 1                   | 1 byte              |

Case 8 then runs `d->inst_length += sign_ext ? 1 : 2`, which **adds to** the decoder's already-counted immediate instead of **replacing** it. The result is that `inst_length` is always overcounted — by the size of the decoder's assumed immediate.

The triage report proposes:

```c
d->inst_length += (sign_ext ? 1 : 2) - (d->operand_width + 1);
```

which subtracts off the decoder's overcount before adding the real size. Verify this is correct for all four opcodes — work through each one by hand, write out the arithmetic in the commit message or a comment. Do not apply the fix until you have independently confirmed it matches the table above.

## Your task

### Phase 1 — Verify the diagnosis

Reproduce the bug before fixing it. Build the current tree, run the emulator against FreeDOS, and confirm it fails in the way the triage report describes (IRET from INT 13 handler lands on wrong instruction because OR `[BP+4],1` advanced IP by 6 instead of 4).

You do not need to instrument anything to see this — the triage report's evidence is sufficient. The point of this phase is to make sure the tree you're working on is the tree the triage report was written against, and nothing has drifted.

If the boot failure does not reproduce as described, **stop and report**. Something has changed since triage and the rest of this brief may be stale.

### Phase 2 — Apply the fix

One-line change in `packages/emu86/src/emulator/run.c`, case 8. Keep the change minimal. Do not refactor surrounding code. Do not "improve" the decoder's length formula elsewhere — that would be scope creep, and the triage specifically noted other latent issues in the decoder that should stay out of this task.

If you find yourself wanting to change more than one line to fix this, stop and explain in the report why the single-line fix doesn't work.

### Phase 3 — Regression tests (this is the real deliverable)

The triage explicitly flagged that 1560 existing tests did not catch Bug 2 because none of them drove the full dispatch path for 0x80–0x83. Closing that gap is as important as the fix itself.

Add unit tests to `packages/emu86/test/unit/test_run.c` that drive each of the four opcodes through `emu86_run()` and assert:

1. **IP advances by the correct amount** after the instruction executes. This is the core failure mode.
2. **The ALU result is correct** (flags and destination value). A length fix that happens to also corrupt execution would be caught by this.
3. **Multiple modrm variants per opcode.** At minimum: register-direct (mod=3), and memory with no displacement, 8-bit displacement, and 16-bit displacement. The bug manifests the same way across these, but a future regression could easily affect only one mode.

Concrete test cases to include (you may add more):

- `80 C0 42 F4` — `ADD AL, 0x42; HLT` → AL = 0x42, IP advance = 3, then HLT
- `81 C0 34 12 F4` — `ADD AX, 0x1234; HLT` → AX = 0x1234, IP advance = 4, then HLT
- `83 C0 01 F4` — `ADD AX, 1; HLT` (sign-extended imm) → AX = 1, IP advance = 3, then HLT
- `83 E8 01 F4` — `SUB AX, 1; HLT` → exercises the `extra` dispatch within the opcode family
- `82 C3 FF F4` — `ADD BL, 0xFF; HLT` → exercises 0x82 specifically (often treated as an alias of 0x80 but must be tested independently)
- At least one test with a memory operand: `81 06 00 02 34 12 F4` or similar (`ADD [0x0200], 0x1234`)

Each test should fail with the current code and pass with the fix. Run the test suite with the fix applied and confirm the new tests pass. Then *temporarily* revert the fix, run the new tests, and confirm they fail — this is how you know they actually exercise the bug. Re-apply the fix before moving on.

Mention in your commit message that you did this revert-and-re-test check. It's the proof that the tests are meaningful.

### Phase 4 — Attempt the boot (bonus, not required)

With the fix and tests in place, try booting FreeDOS:

```
cd packages/emu86
./emu86 reference/bios test/images/freedos.img
```

Three possible outcomes:

**(a) FreeDOS boots to `A:\>`.** Major milestone. Note it in the commit message. Do not celebrate by committing a second bug fix in the same commit — if you spot something else that needs doing, it's the next task.

**(b) FreeDOS fails differently than before.** This is likely the register-memory mapping gap at 0xF0000 that the triage report flagged as "may surface once Bug 2 is fixed." Characterise the new failure briefly in your commit message, note it as follow-up work, but **do not fix it here**. That is a separate task.

**(c) FreeDOS fails the same way as before.** Something is wrong with either the fix or your understanding of the bug. Stop and report — do not commit.

Phase 4 is diagnostic, not acceptance. The acceptance criterion is Phase 3, not Phase 4.

### Phase 5 — Commit (on success) or report (on failure)

**If Phases 1, 2, and 3 all succeed** (fix applied, regression tests pass, revert-and-re-test proven), commit following this sequence.

First, update the task log. Append to `tasks/completed/task-log.md`:

```
## EMU-13
Date: {today's date}
Status: PASS
Test results: {X passed, Y failed} — including N new tests for 0x80-0x83 ALU-imm length
Boot test: {FreeDOS boots / FreeDOS fails with new symptom / FreeDOS fails unchanged}
Notes: Fixed ALU-imm instruction length overcount for opcodes 0x80-0x83 in run.c case 8. Root cause: decoder's linear length formula counts `iw_size * (operand_width + 1)` bytes for immediate, then case 8 adds 1 or 2 more bytes instead of replacing. Fix: subtract decoder's overcount before adding real immediate size. Added N regression tests in test_run.c covering all four opcodes with register-direct and memory-operand modrm variants. Verified tests fail against unpatched code (revert-and-re-test). {Brief note on boot test outcome.}
```

Then move the task file and commit:

```bash
mv tasks/emu13-task.md tasks/completed/
git add -A
git commit  # paste the message from your self-audit below
git push
```

**The commit message must be a self-audit.** Not a one-liner. Structure:

```
EMU-13: Fix ALU-immediate instruction length (0x80-0x83)

Bug: [one paragraph on what was wrong and why]

Fix: [the single-line change, quoted, plus one sentence on why it works
      for each of the four opcodes]

Tests: [N new tests added, what they cover, confirmation that they fail
        without the fix]

Boot test: [FreeDOS outcome — boots / fails differently / not attempted and why]

Follow-up: [anything surfaced that becomes the next task]
```

If you cannot honestly write all five of those sections, do not commit. Stop and write a report instead.

**If Phase 1 fails, or Phase 2 can't be done in a single line, or Phase 3 tests can't be made to both fail-before and pass-after, or Phase 4 produces outcome (c):** stop and write a report to `tasks/triage/emu13-report.md`. Do not commit. Use the same report structure as the EMU-12 triage report (Summary, Evidence, Root cause, Changes made, Remaining concerns, Recommended next step).

## Out of scope — do not touch

The EMU-12 triage report listed four "Remaining concerns" beyond Bug 2. None of them are in scope for EMU-13:

- `i_mod_size` treated as boolean rather than multiplier (latent, not currently triggered)
- Register-memory mapping gap at 0xF0000 (may surface during Phase 4 — if it does, note it, don't fix it)
- Silent exit on CS:IP = 0:0 with no output (UX/debuggability improvement)
- Prior EMU-12 task-log entry being misleading (already addressed in the EMU-12 commit)

If you notice any of these (or anything else) during the work, note them in the commit message's "Follow-up" section or in your report's "Remaining concerns." Do not fix them.

Similarly, the `editor-api-proposal.md` in `packages/emu86/docs/` is an unratified proposal. It is not yours to ratify, modify, or act on.

## Final note on honesty

This is a narrower task than the EMU-12 triage, but it carries the same risk the triage was written to prevent: an agent can apply a fix that passes tests without genuinely understanding why, and the test suite can be sloppy enough to not catch a subtly-wrong fix. The revert-and-re-test check in Phase 3 is the specific safeguard against that. The self-audit commit message is the broader safeguard.

Writing "I did X and it worked" is easy. Writing "here is the arithmetic for each of the four opcodes, here are the tests that would have caught the bug, here is the proof they actually exercise it" is the job.

If at any point you're uncertain — about the fix, about whether your tests are adequate, about what the boot failure means — stop and report. A partial deliverable with honest uncertainty is worth more than a confident-sounding commit that glosses over what didn't get verified.
