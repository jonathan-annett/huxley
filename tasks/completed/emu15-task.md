# EMU-15: Fix 0xC0/0xC1 instruction-length overshoot

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and two triage reports at `tasks/triage/`. Those are *reference material*, not your assignment. Phase 1 of this brief asks you to read specific parts of them. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

Recent history:

- EMU-12 committed the Linux host (PARTIAL — boot blocked).
- EMU-13 fixed an instruction-length off-by-one in `run.c` case 8 for opcodes 0x80–0x83. Boot advanced from 209 to ~156,246 instructions.
- EMU-14 was a triage task that investigated the remaining boot failure. It falsified the working hypothesis (register-memory aliasing) and instead identified a structurally identical bug in `run.c` case 12 for opcodes 0xC0/0xC1 (shift/rotate r/m, imm). The triage report explicitly declined to fix the bug, per EMU-14's scope rules, and recommended a new task modelled on EMU-13.

**This is that task.** It is narrow: fix the 0xC0/0xC1 length bug, add regression tests, audit two related cases for sibling bugs, and attempt a boot.

## Required reading before you touch code

1. **`tasks/triage/emu14-triage-report.md`** (may also be named `emu14-report.md`) — the full triage investigation. The "Root cause" and "Recommended next step" sections give you the exact fix and testing approach. The "Remaining concerns" section lists two additional cases worth auditing — that's Phase 3 of this task.
2. **`tasks/completed/emu13-task.md`** — the immediate precedent for this task. The shape of the fix, the revert-and-re-test discipline, and the commit-message self-audit structure were all established there. You are doing the same thing for a different opcode family.
3. **`packages/emu86/src/emulator/run.c`**, specifically case 12 — the buggy code. Study how `d->inst_length` is computed at decode time and then incremented at execute time. Compare against case 8 (which EMU-13 fixed) to see the pattern.
4. **`packages/emu86/src/emulator/decode.h`** or wherever the linear length formula lives — understand `base_size * ... + iw_size * (operand_width + 1)` and what values the BIOS tables supply for opcodes 0xC0 and 0xC1.

## The bug in plain terms

For opcodes 0xC0 and 0xC1 (shift/rotate r/m, imm — the 80186 shift-by-immediate forms), the BIOS tables report `base_size = 3`. That 3 already accounts for opcode + modrm + imm8. So the decoder's linear formula computes `inst_length = 3` (plus modrm displacement if any), which is correct.

The dispatch code in `run.c` case 12 then adds:

```c
if (d->extra) {
    count = (uint8_t)(d->data1 & 0xFF);
    d->inst_length++;   // <-- over-advances by 1
}
```

That `++` makes `inst_length = 4` for a 3-byte instruction, causing IP to skip a byte on every 0xC0/0xC1 execution. The BIOS hits this at `F000:0D4D` executing `C1 E0 09` (SHL AX, 9), skips the following `0F` byte (first byte of the emulator-specific `extended_read_disk` hook), and the skipped instruction cascades into the 156,246-instruction silent-exit failure that EMU-14 characterised.

The reference 8086tiny avoids this through a different mechanism: its OPCODE 12 handler calls `set_opcode(0x10)` which remaps the opcode to ADC's tables (base_size=2), so that when its post-execute IP advance runs it uses 2+1=3. Our refactor dropped that remap but kept the `++`, which is why the numbers no longer balance.

**The fix is to delete the `d->inst_length++;` line.** That's it. One line.

## Your task

### Phase 1 — Verify the bug is still present

Build the current tree, confirm FreeDOS still fails at ~156,246 instructions as described in the EMU-14 triage report. If the behaviour has drifted since triage (for example, someone committed something unexpected), stop and report.

```
cd packages/emu86
make
./emu86 reference/bios test/images/freedos.img
```

You do not need to instrument anything for this phase. The EMU-14 triage already characterised the failure in depth; your job is to confirm it still reproduces.

### Phase 2 — Apply the fix

Remove the `d->inst_length++;` line from case 12 in `packages/emu86/src/emulator/run.c`. Keep the change minimal. Do not refactor surrounding code. Do not "improve" the broader length-calculation logic — that is explicitly out of scope (see Phase 3 for the controlled exception).

If you find yourself wanting to change more than one line, stop and explain in the report why a single-line fix doesn't work.

### Phase 3 — Audit two related cases (controlled scope expansion)

The EMU-14 triage flagged two additional cases in `run.c` as "potential siblings of the same bug." The scope of EMU-15 includes a **brief, read-only audit** of these two cases — enough to know whether they're broken, not enough to fix them here.

Cases to audit:

1. **Case 6 sub 0 (TEST r/m, imm — opcodes 0xF6 / 0xF7 with `extra=0`).** The triage noted it "adds `d->operand_width + 1` to `d->inst_length` for the immediate." Check whether `base_size` and `iw_size` for 0xF6/0xF7 already count that immediate, making the addition a duplicate (the bug pattern) or a necessary adjustment (correct).

2. **Case 14 (JMP/CALL near/far — opcodes 0xE8 / 0xE9 / 0xEA / 0xEB and friends).** The triage noted it "sets `d->inst_length = 3 - d->direction` manually, overriding the decoder's value." Check whether this override is correct across all the opcodes it handles, or whether there's a variant where it's wrong.

For each of the two, the audit produces one of three results:

**(a) Correct** — the per-case adjustment is compensating for something real and the length comes out right. Document why, briefly.

**(b) Broken but not currently triggered** — the bug exists but the BIOS (or your existing tests) never exercise the broken path. Document the specific conditions under which it would break and flag as follow-up work.

**(c) Broken and likely to trigger during FreeDOS boot.** If audit finds this, *then* you have a legitimate scope expansion: fix that case with the same discipline (minimal change, regression test, revert-and-re-test) before proceeding to Phase 4. This is the only controlled exception to the "one fix per task" rule.

If (c) doesn't apply, do not fix these cases in EMU-15. Note the audit findings in the commit message's "Follow-up" section.

**Do not expand the audit beyond these two cases.** The EMU-14 triage flagged specifically these; a broader audit is its own future task (likely a differential-test harness against the reference emulator, which is already under discussion at the project-plan level).

### Phase 4 — Regression tests

Add unit tests to `packages/emu86/test/unit/test_run.c` that drive opcodes 0xC0 and 0xC1 through `emu86_run()` and assert IP advanced by the correct amount. Concrete tests to include:

- `C1 E0 09 F4` — `SHL AX, 9; HLT` → AX = 0 (shifted out), IP advance = 3 before HLT. This is the exact instruction that was triggering the BIOS failure.
- `C1 E8 01 F4` — `SHR AX, 1; HLT` → exercises SHR instead of SHL (different `extra` value within case 12)
- `C0 C0 04 F4` — `ROL AL, 4; HLT` → 8-bit form (0xC0 rather than 0xC1)
- At least one test with a memory operand: `C1 26 00 02 04 F4` or similar (`SHL word [0x0200], 4; HLT`) — exercises the path where modrm displacement adds to inst_length

If Phase 3 found case (c) for case 6 sub 0 or case 14, add regression tests for the specific bug found there too, following the same pattern.

**Revert-and-re-test discipline.** For each new test, confirm it fails against unpatched code before the fix, then passes after. This is the same check EMU-13 used. Mention in the commit message that you did it.

Run the full test suite after applying the fix and confirm everything still passes. The fix should not break any existing test.

### Phase 5 — Attempt the boot

With the fix and tests in place:

```
./emu86 reference/bios test/images/freedos.img
```

Three outcomes:

**(a) FreeDOS reaches the `A:\>` prompt.** Big milestone. Note it in the commit. Do not celebrate by slipping in extra changes — anything else you notice is the next task.

**(b) FreeDOS fails further into the boot than before.** The EMU-14 triage's own Phase 2 independent verification observed kernel banner print followed by "Interrupt divide by zero." So outcome (b) is expected, not a surprise. Characterise the new failure briefly in your commit message and note it as follow-up. Do not fix it here.

**(c) FreeDOS fails at the same 156,246-instruction point as before.** The fix did not solve the problem. Do not commit — stop and report.

Phase 5 is diagnostic. The acceptance criterion for commit is Phase 4 (fix plus regression tests), not Phase 5.

### Phase 6 — Commit (on success) or report (on failure)

**Commit** if and only if: Phase 1 confirmed the bug is still present, Phase 2 applied the one-line fix, Phase 3's audit completed with documented findings, Phase 4's regression tests both fail-before and pass-after, and Phase 5 produced outcome (a) or (b).

Commit message structure — same self-audit discipline as EMU-13, with an added section for the Phase 3 audit:

```
EMU-15: Fix shift/rotate-imm instruction length (0xC0/0xC1)

Bug: [one paragraph on what was wrong and why, referencing EMU-14 triage]

Fix: [the one-line change, quoted, plus arithmetic showing it produces the
      correct inst_length for both 0xC0 and 0xC1]

Audit (Phase 3):
  - Case 6 sub 0 (TEST r/m, imm): [one sentence — correct / broken-latent /
    broken-and-fixed-here]
  - Case 14 (JMP/CALL): [one sentence — correct / broken-latent /
    broken-and-fixed-here]

Tests: [N new tests added, what they cover, revert-and-re-test confirmation]

Boot test: [outcome (a) or (b), with specifics — how far did FreeDOS get,
            what was the failure mode if (b)]

Follow-up: [anything surfaced that becomes the next task, including any
            broken-latent findings from the audit]
```

Task log entry appended to `tasks/completed/task-log.md`:

```
## EMU-15
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests for 0xC0/0xC1 length
Boot test: {outcome}
Notes: Fixed 0xC0/0xC1 instruction-length overshoot in run.c case 12 (removed
spurious d->inst_length++). Same bug pattern as EMU-13's case 8 fix. Audit of
two related cases flagged by EMU-14 triage: [brief results]. {Boot outcome
summary.}
```

Then:

```bash
mv tasks/emu15-task.md tasks/completed/
git add -A
git commit  # paste the self-audit message
```

Do **not** push. The user will review and push manually.

**Report** to `tasks/triage/emu15-triage-report.md` if: Phase 1 can't reproduce the bug, Phase 2 needs more than one line, Phase 3 uncovers a case-(c) issue that genuinely can't be localised, Phase 4 tests can't be made to fail-before-and-pass-after, or Phase 5 produces outcome (c).

Use the same report structure as prior triage reports (Summary, Evidence, Root cause, Scope assessment, Changes made, Remaining concerns, Recommended next step). Leave the tree clean.

## Out of scope — do not touch

- The broader "does the length formula have edge cases in cases other than 6, 12, and 14" question. That is destined to become a differential-testing task, not part of EMU-15.
- Fixing the "Interrupt divide by zero" that EMU-14 saw during its independent verification — that's whatever comes after EMU-15.
- The register-memory aliasing concern from EMU-12 triage. Still open, still not this task.
- The silent-exit-on-0:0 debuggability issue. Still open, still not this task.
- The `editor-api-proposal.md` in `packages/emu86/docs/`. Still unratified, still not this task.

If you notice any of these or anything else during the work, note them in your commit's "Follow-up" section or your report's "Remaining concerns." Do not fix them.

## Housekeeping from EMU-14

The prior session left an untracked binary `packages/emu86/emu86-dbg` because `rm` is not in its permission allow-list. If it is still present when you start, note its existence but **do not delete it** — that's the user's job. It should not end up in the commit (it's covered by `.gitignore` via `packages/emu86/emu86` — verify this is still the case, or extend `.gitignore` if needed).

Actually: check `.gitignore` treatment before doing anything else. If the binary shows up in `git status` as untracked, it needs a gitignore entry. The user has told me modifying `.gitignore` is something to be careful about — if a change is needed, make the smallest possible change (adding `packages/emu86/emu86-dbg` specifically, not a broader glob) and flag it explicitly in the commit message.

## Final note on honesty

This is a well-understood task. The EMU-14 triage did the investigation; your job is execution plus a small controlled audit. The risk here is not confirmation bias (as in EMU-14) or novel bug hunting (as in EMU-12 triage) — it's *complacency*. A one-line fix feels like it can't go wrong. But the tests are the real deliverable, not the fix. Sloppy tests that happen to pass without exercising the bug would be the failure mode.

Revert-and-re-test is what proves the tests have teeth. Document that you did it in the commit. If at any point you're uncertain, stop and report.
