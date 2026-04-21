# EMU-16: Differential test harness (lockstep against reference)

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

Recent history has surfaced a pattern: EMU-13 and EMU-15 both fixed structurally identical bugs (instruction-length calculation in `run.c` dispatch cases) that unit tests did not catch. Each bug was discovered by booting FreeDOS until it failed, then instrumenting to find the cause. That cycle is expensive (tens of thousands to hundreds of thousands of instructions of boot before a bug manifests) and non-exhaustive (the next bug only surfaces after the current one is fixed).

This task is a pivot from bug-fixing to tool-building. The goal is to build a **differential test harness** that runs our emulator in lockstep with the reference `8086tiny` emulator, comparing CPU state after every instruction, halting at the first divergence. This converts "boot FreeDOS and hope a bug shows up" into "run N instructions and the harness tells you which instruction diverged and exactly how."

The expected payoff: known and unknown bugs of the EMU-13/EMU-15 class become trivially findable, and future regressions become impossible to introduce undetected.

## The design, in broad strokes

**One process, one event loop, two CPUs stepped in lockstep.** No threads, no IPC, no concurrency. At each iteration:

1. The reference executes one instruction (via its existing `sim()` main loop).
2. Our emulator executes one instruction (via the existing `emu86_step_single()` function).
3. The harness compares CPU state between the two.
4. On divergence: halt with a detailed report. On match: continue.

**Integration with the reference is via macros, not modification.** Adrian Cable's `8086tiny.c` (in `packages/emu86/reference/`) already uses compile-time flags (`NO_GRAPHICS`, `NO_AUDIO`) as its extension mechanism. We follow the same pattern.

Specifically, two call sites are inserted into `sim()`'s main loop — call them `BEGIN_STEP()` and `END_STEP()` — that expand to empty macros by default (so the reference still builds and runs standalone unchanged) and to harness-provided bodies when compiled for the harness. `BEGIN_STEP()` can record timing data; `END_STEP()` is where the actual per-step lockstep work happens (advance our emulator, compare, halt on divergence).

**Reference state is globals.** 8086tiny stores CPU state (`reg_ax`, `seg_cs`, `mem`, flags, etc.) in file-scope globals, not in a struct. The macros expand inline within `sim()`, so they can reference these globals directly. The harness reads them via `extern` declarations. No state-bundling struct is needed on the reference side.

**Our state is a struct.** `Emu86State` is accessed normally. The comparator reads fields from one side and globals from the other.

**What counts as "state" comes from ORIGINAL-ANALYSIS.md.** EMU-01 produced `packages/emu86/docs/ORIGINAL-ANALYSIS.md`, which classifies every global in 8086tiny as either CPU state (snapshot-worthy, should be compared) or scratch/ephemeral (not state, should not be compared). Use that classification — do not re-derive it.

## Non-determinism — the critical design constraint

For lockstep to be meaningful, both emulators must be fully deterministic — given identical initial state and identical inputs, they must produce identical per-instruction behaviour. Any source of non-determinism in either emulator will produce false divergences that look like bugs but aren't.

Sources of non-determinism to investigate and neutralise:

**1. Timer / RTC.** The reference probably updates its PIT (timer) counter based on wall-clock time. If ours does the same at a different rate (because harness overhead makes one side slower), timer-driven behaviour diverges. Solution: make timer tick deterministic — e.g., "tick every N instructions" rather than "tick every M milliseconds of wall-clock." This may require an additional compile-time override of the reference's timer mechanism, in the same macro-based style as `BEGIN_STEP`/`END_STEP`.

**2. Keyboard / input.** Neither emulator can read the host keyboard independently during a lockstep run. **For v1, the harness delivers zero keystrokes** — it runs from cold boot until either a divergence is found or a bounded end condition is reached (see "Success criteria" below). If the reference's `sim()` polls the host keyboard as part of its main loop, that polling must be neutralised — either compile the reference with an override that returns "no key pressed," or gate the poll behind a macro that does nothing in the harness build.

**3. Disk reads.** Both emulators should read from the same backing file (`test/images/freedos.img`), and `read()` from a regular file is deterministic. This one is probably free. Verify.

**4. Memory initialisation.** Both sides must start with identical memory contents — same BIOS loaded at the same address, same zeroed regions elsewhere. Probably already the case, verify.

**5. Host-side I/O side effects.** The reference probably writes to stdout (console output) as a side effect of BIOS INT 10h. So does ours. Both writes are fine — they're logically identical outputs from logically identical states. But the harness must not let one side block on I/O while the other doesn't (e.g., if one uses buffered I/O and the other doesn't).

**If you find a source of non-determinism that cannot be neutralised via compile-time macros or harness-provided replacements, stop and report.** Neutralising by refactoring the reference's core logic is out of scope for v1 of this harness.

## Your task

### Phase 1 — Investigate the reference

Read `packages/emu86/reference/8086tiny.c`. Understand:

1. Where is the main loop (`sim()` or equivalent)? What does one iteration do?
2. What globals does it touch per iteration? Which are CPU state and which are scratch? (Cross-reference against `ORIGINAL-ANALYSIS.md`.)
3. What sources of non-determinism exist per iteration? Timer, keyboard poll, anything else.
4. How is the reference currently compiled? What `-D` flags are used in the existing build? What flag-gated code paths exist?

Also look at our emulator's `emu86_step_single()` (defined in `packages/emu86/src/emulator/run.c`) to confirm it executes exactly one instruction and is safe to call repeatedly.

Document your findings at the start of your work session — either in the commit message if you commit, or in the report if you report.

### Phase 2 — Design

Decide, based on Phase 1 evidence:

1. **Where `BEGIN_STEP()` and `END_STEP()` go in `sim()`** — typically at the start and end of each main-loop iteration, enclosing one instruction's worth of work.
2. **How to neutralise timer non-determinism** — likely a second macro (e.g., `TIMER_TICK()`) or a compile-time override of the timer read.
3. **How to neutralise keyboard non-determinism** — likely a macro or a stub function.
4. **How state comparison is structured** — which reference globals to compare, which our-state fields they correspond to, how memory is compared (full memcmp each step is acceptable for v1; write-tracking is optimisation).
5. **Where the harness code lives** — probably a new directory `packages/emu86/harness/` containing its own Makefile target that compiles the reference with the harness macros defined, links against our emulator, and produces a `harness` binary.

**Constraint: minimal modification to `reference/8086tiny.c`.** Ideally, two or three macro call-site insertions and nothing else. If your design requires more, justify it. A diff of more than ~10 lines to the reference is a flag that the approach may be wrong.

**The default expansions of `BEGIN_STEP` / `END_STEP` / `TIMER_TICK` / any other inserted macros must be empty.** Compiling the reference without the harness flags must produce a binary byte-identical (or nearly so — one `const`-folded empty-statement aside) to what it produces today. Test this: compile before and after your macro insertions, confirm the reference still builds and runs standalone.

### Phase 3 — Implementation

Write the harness. It should:

- Compile the reference with harness macros defined (specifics depend on your Phase 2 design).
- Initialise both emulators identically — same BIOS at same address, same disk image, zeroed memory elsewhere.
- Run in lockstep: reference steps, our emulator steps, compare, repeat.
- On divergence, produce a report:
  - Instruction count at divergence
  - CS:IP on both sides (they may still match even if state diverged, if the bug is in execution rather than length)
  - The opcode bytes at CS:IP
  - Which state fields differ and their values on each side (registers, flags, individual memory bytes or regions)
- On clean run to end condition, report success with instruction count and bounded-exit reason.

The comparator should distinguish "persistent state" (compare) from "scratch/ephemeral" (ignore), per the Phase 2 design. It is better to err on the side of comparing too much than too little in v1 — false divergences are informative (they tell us what we need to exclude), whereas missing a real divergence would be a correctness failure of the tool.

### Phase 4 — Test the harness itself

Before running it against FreeDOS, verify the harness doesn't lie.

1. **Self-consistency test.** Configure the harness to compare a fresh state against itself (or a known-identical copy). It should report zero divergences across many instructions.

2. **Injected-divergence test.** Deliberately perturb one emulator's state (e.g., set AX to a different value after one step) and confirm the harness detects and reports the divergence correctly, with correct diagnostic information.

These two tests are the minimum viable proof that the tool works. Without them, a "no divergence found" result from a real run is meaningless.

### Phase 5 — Run against FreeDOS

Run the harness against the FreeDOS boot sequence:

```
packages/emu86/harness/harness reference/bios test/images/freedos.img
```

(Exact invocation depends on your Phase 2 design.)

The expected outcomes, in rough order of likelihood:

**(a) Divergence detected early** — a bug we haven't found yet (or one we have, if our current tree still has it). The report should identify the instruction and the diverging state. Document the divergence; **do not fix the bug** (that's the next task). A divergence is a successful test run — it means the tool works.

**(b) Clean run to divide-by-zero** — FreeDOS boots to the known divide-by-zero failure, no divergence detected before that point. This means our emulator matches the reference exactly for the first ~156,000+ instructions, and the divide-by-zero is either a bug in both emulators (probably not — the reference presumably boots successfully) or something else the harness isn't catching (worth investigating).

**(c) Divergence at a point the reference also fails** — the reference emulator hits a problem at some instruction, and our emulator diverges from it there. This might be a bug in our neutralisation of non-determinism (did the reference's keyboard poll fall through to something unexpected?) or a legitimate divergence coincident with a reference-side issue. Diagnose carefully.

**(d) Something broken** — the harness itself fails to run, produces no useful output, etc.

For outcomes (a) and (b), the task is a success and should commit. For (c) and (d), diagnose the harness before drawing conclusions about emulator correctness.

### Phase 6 — Commit (on success) or report (on failure)

**Commit** if: the harness builds, Phase 4's self-tests pass (zero divergences on identical states; correct detection of injected divergences), and Phase 5 produces outcome (a) or (b). The divergence (if any) is reported but not fixed.

Commit message structure:

```
EMU-16: Differential test harness (lockstep against reference)

Approach: [brief description of the macro-based integration and why]

Reference modifications: [exact diff summary — e.g., "3 macro call-site
                         insertions in sim() around the main loop body"]

Harness structure: [what lives where, build integration]

Non-determinism handling: [how timer, keyboard, etc. are neutralised]

Self-tests: [description of Phase 4 tests and their results]

First real run: [Phase 5 outcome — what happened when run against FreeDOS,
                instruction count reached, divergence (if any) summarised]

Follow-up: [the divergence found (if any) becomes the next bug-fix task;
            any design limitations worth flagging; anything discovered but
            out of scope]
```

Task log entry appended to `tasks/completed/task-log.md`:

```
## EMU-16
Date: {today}
Status: PASS
Test results: {harness self-tests: passed / failed}
Harness run: {instruction count reached, divergence found (if any)}
Notes: {summary of approach, key design decisions, first divergence finding}
```

Then move the task file, commit (do not push):

```bash
mv tasks/emu16-task.md tasks/completed/
git add -A
git commit
```

**Report** to `tasks/triage/emu16-triage-report.md` if: non-determinism sources cannot be neutralised via macros (Phase 2 scope question), the harness self-tests fail (Phase 4), Phase 5 produces outcome (c) or (d) in a way you can't diagnose, or the design requires more than ~10 lines of modification to the reference.

Use the same report structure as prior triage reports.

## Out of scope — do not touch

- **Fixing any bug the harness finds.** If divergence is detected, report it. The next task fixes it.
- **v2 features of the harness.** Keystroke delivery, mouse events, synthetic test programs, ELKs support, snapshot comparison at checkpoints — all future work. v1 is cold-boot-to-FreeDOS-endpoint lockstep against a deterministic input stream (empty, effectively).
- **Performance optimisation.** If the harness runs at 10% of real-time, that's fine. v1 is about correctness of the tool, not speed. Document performance as observed; don't tune.
- **Refactoring either emulator.** The whole point of the harness is that it compares unmodified behaviour. If you find yourself wanting to refactor `emu86_step_single()` to make the comparison easier, that's a scope violation — find a way that doesn't require that.
- Other latent concerns from prior triages (register-memory aliasing, silent-exit-on-0:0, 0xEA JMP/CALL latent length bug). Not this task.
- The `editor-api-proposal.md`. Still unratified, still not this task.

If you notice anything worth raising, put it in the Follow-up section of the commit or the Remaining-concerns section of the report.

## Housekeeping

The prior EMU-14 session left an untracked binary `packages/emu86/emu86-dbg`. If still present, leave it; don't delete. Your own debug binaries during investigation should also be left behind (the user can clean up), but make sure none end up in the commit — check `.gitignore` handles them, extend only if necessary with minimal-glob scope and flag the change explicitly in the commit message.

## Final note

This is a tooling task, not a bug-fix task. The failure modes are different:

- EMU-13 failure mode would have been "sloppy fix on a simple bug" — mitigated by revert-and-re-test.
- EMU-15 failure mode would have been "complacency on an easy task" — mitigated by the Phase 3 audit.
- EMU-16's failure mode is *scope creep on an inherently larger task*. Building a differential test harness is more open-ended than fixing an opcode, and there are many ways to spend an afternoon "improving" it beyond v1. The tight scope definition — cold boot to endpoint, no keystrokes, macro-based integration, minimal reference mods, halt-on-divergence-without-fixing — is load-bearing. If you find the v1 scope insufficient to demonstrate the tool works, that's a report, not a silent expansion.

Also: this tool, if it works, probably finds unknown bugs on its first real run. That is a successful outcome, not a setback. Report the divergence with precision; let the next task fix it. The harness's value is in finding things reliably, not in self-justifying by claiming a successful boot.
