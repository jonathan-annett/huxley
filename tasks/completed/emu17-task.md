# EMU-17: Harness disk isolation (scratch files per emulator)

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

EMU-16 built a differential test harness that runs our emulator in lockstep with the reference `8086tiny`, comparing CPU state after every instruction. It lives at `packages/emu86/harness/`. On its first real run it detected a divergence at instruction 4096 — a timer-cadence bug in our emulator — exactly as intended.

**EMU-16 has a capability gap that must be closed before the harness is used more broadly.** Both emulators currently open the same disk image file (`test/images/freedos.img`). For the cold-boot-to-divide-by-zero workload used in EMU-16, no disk *writes* occur, so this has worked in practice. But any workload that writes to disk would:

1. Corrupt the shared source image silently, invalidating subsequent harness runs.
2. Create a race — both emulators writing to the same file — that could produce false divergences or mask real ones.
3. Make disk writes *non-comparable* — we couldn't see "the reference wrote X, ours wrote Y" because only one write would survive to disk.

This task closes that gap. Each emulator gets its own scratch copy of the disk image, opens that copy for read/write, and the scratch files are preserved as outputs for offline comparison.

## The design

**At harness startup**, before either emulator begins initialising:

1. Copy `test/images/freedos.img` to two scratch files, for example `test/images/freedos.img.ref.scratch` and `test/images/freedos.img.hux.scratch`. Exact path and naming is a design choice — the brief doesn't mandate a specific scheme, but they should be *distinguishable* and *not in `test/images/`* if that would pollute the tracked inputs directory (consider `harness/work/` or `/tmp/` or a similar scratch location; decide in Phase 2).
2. Hand each scratch path to the appropriate emulator's initialisation. The reference gets `ref.scratch`; our emulator gets `hux.scratch`. Neither ever sees the original.

**At runtime**, each emulator does its disk I/O — reads, writes, seeks — against its own scratch file. The harness does *not* intercept these I/O calls. The existing `overrides.c` machinery is for non-determinism (stdin reads, timer calls), not for disk redirection. Disk redirection is handled by path substitution at initialisation, not by runtime syscall interception.

**At harness shutdown** (whether by divergence, step-limit reached, or clean completion), the scratch files are the deliverables. Leave them in place, or rename them to `.ref.out` / `.hux.out`, so they can be inspected or diffed offline.

## Required reading before you touch code

1. **`packages/emu86/harness/harness.c`** — the existing harness. Understand its main loop, how it initialises each emulator, how it hands the disk image path to each side.
2. **`packages/emu86/harness/overrides.c`** — the deterministic stubs for `read`, `time`, etc. Read it to understand the current non-determinism strategy, but note: you are **not** expected to add disk-I/O redirection here. The design explicitly avoids that.
3. **`packages/emu86/reference/8086tiny.c`** — specifically, how it acquires its floppy disk file handle. Look for `open()` calls and argument parsing near `main()` / `sim()` entry. The design hinges on whether the reference opens the floppy via a path passed from the command line (cleanest case — we just pass a different path) or via a hardcoded/internal mechanism (requires extra care).
4. **`packages/emu86/src/hosts/linux/main.c`** — how our emulator acquires its floppy disk file handle, for the same reasons.

The outcome of this reading determines the exact mechanism in Phase 2. Don't commit to an implementation before you know what each emulator expects.

## Your task

### Phase 1 — Investigate disk-handle acquisition

Read the relevant files listed above and determine, for each emulator:

- How does it currently receive the disk image path? (argv, env var, compiled-in constant, other)
- How and where does it `open()` the file? What mode (read-only, read-write)?
- What file descriptor does it use for subsequent reads/writes? Is this a property of `Emu86Platform` on our side, or a global on the reference side?

Document your findings briefly in the commit message's "Approach" section. The chosen design in Phase 2 should flow from these findings.

### Phase 2 — Design

Decide the scratch-file mechanism based on Phase 1 evidence. Possibilities in rough preference order:

**Option A — Path substitution via argv.** If both emulators receive their disk image path as a command-line argument, the harness's `main()` copies the image to two scratch files and passes the scratch paths in argv to each emulator's initialisation. No `open()` interception needed. Simplest, most transparent.

**Option B — Platform-layer injection.** For our emulator specifically, `Emu86Platform` already abstracts disk I/O behind function pointers. The harness can pass a platform-layer setup that opens the hux scratch file. Applies to our side regardless of whether the reference uses Option A.

**Option C — `open()` override.** Intercept `open()` calls in overrides.c and redirect the original image path to the appropriate scratch path. Use only if neither A nor B is viable on one side. Least transparent — reserve as a fallback.

**Constraint:** whichever mechanism you choose, the reference binary when built standalone (without harness macros) must still behave identically to its pre-EMU-17 state. Verify this the same way EMU-16 verified it: md5 the standalone reference build before and after your changes and confirm they match. If your design requires any modification to `reference/8086tiny.c` beyond the harness-macro insertions already there from EMU-16, justify it explicitly and keep the diff minimal.

Decide also:

- **Scratch file location.** Outside `test/images/` to avoid polluting the tracked inputs directory. `harness/work/` or `/tmp/` are reasonable choices. If you choose a location inside the repo, add the directory to `.gitignore` (with the minimum-glob discipline established in prior tasks — e.g., `packages/emu86/harness/work/` specifically, not a broad glob).
- **Scratch file lifetime.** Options: (a) create fresh at start, delete at end; (b) create fresh at start, preserve at end for inspection; (c) preserve across runs and overwrite. My recommendation: (b). Fresh at start ensures each run starts identical; preservation at end means divergence can be inspected offline. Document the choice.
- **Naming.** The `.ref.scratch` / `.hux.scratch` suggestion in this brief is only a suggestion. Pick names that are unambiguous and don't clash with anything else.

### Phase 3 — Implementation

Write the code to:

1. Copy the source image to the two scratch files at harness startup. Use a robust copy (not `system("cp ...")`) — either `sendfile`, or open-and-read-write-in-chunks, or equivalent. Handle errors explicitly (disk full, permission denied, source missing).
2. Ensure each emulator opens its own scratch file, not the source. Per Phase 2's chosen mechanism.
3. At harness shutdown (all code paths — divergence, step-limit, error exit), the scratch files should be in a known state as designed in Phase 2.
4. Update the harness's user-facing output to mention the scratch file paths so they can be found after a run.

**Constraint:** `overrides.c` should remain the non-determinism layer only. Do not add disk-I/O interception there. If your design requires syscall-level interception of disk I/O (Option C fallback), that's a signal to double-check whether A or B could have worked.

### Phase 4 — Self-tests

Before declaring the task complete, verify two things concretely:

**Self-test 1 — Source image is untouched.**

1. Before running the harness: compute `md5sum test/images/freedos.img`.
2. Run the harness (any workload — cold boot is fine).
3. After the harness exits: compute `md5sum test/images/freedos.img` again.
4. The two checksums must be identical. If not, the scratch mechanism isn't isolating writes properly.

**Self-test 2 — Divergent writes are captured.**

Construct or arrange a scenario where the two emulators would legitimately write different bytes to disk, and confirm the two scratch files differ afterward. A clean way to do this: use the `HARNESS_INJECT_DIVERGENCE_AT` mechanism from EMU-16 (or an analogous injection) to force the two emulators to diverge at an instruction that happens to be followed by a disk write, then inspect the scratch files and confirm they differ in the expected location.

If constructing such a scenario is non-trivial, fall back to: run the harness long enough that at least one disk write has plausibly occurred (document how you verified a write occurred), then confirm *both* scratch files differ from the source *and* are identical to each other (for a matching run) or differ from each other (for an injected-divergence run).

If neither flavour of Self-test 2 can be made to work cleanly, document it as a limitation in the commit message and skip. Self-test 1 is the mandatory one.

### Phase 5 — Run against FreeDOS

With the scratch-file mechanism in place, re-run the cold-boot lockstep that EMU-16 did:

```
./packages/emu86/harness/harness reference/bios test/images/freedos.img
```

(Adjust invocation based on whatever arg structure you chose in Phase 2.)

Expected outcome: the same result as EMU-16 — divergence detected at instruction 4096 (the timer-cadence bug). This is your control: EMU-17 should not change *what* the harness finds, only *how safely* it operates. If the result differs from EMU-16's finding, something in your Phase 3 work has altered the harness's CPU-state comparison path — that's a regression that needs investigation before commit.

Confirm the source image is still byte-identical after this run (Self-test 1 again, end-to-end).

### Phase 6 — Commit (on success) or report (on failure)

**Commit** if: Phase 2's chosen mechanism is defensible (A or B, not C), Phase 3 implemented it cleanly, Phase 4's Self-test 1 passes (source image untouched), Phase 5 reproduces EMU-16's divergence finding, and the reference's standalone build is still md5-identical to its pre-EMU-17 state.

Commit message structure:

```
EMU-17: Harness disk isolation (scratch files per emulator)

Approach: [brief description of Phase 2's chosen mechanism and why]

Phase 1 findings: [how each emulator acquired its disk handle, one
                   sentence each]

Scratch mechanism: [paths used, lifetime policy, how each emulator
                    receives its path]

Reference delta: [any new modification to reference/8086tiny.c beyond
                  EMU-16's macros, with justification — should be zero
                  under Option A or B]

Self-tests: [Self-test 1 result; Self-test 2 result or documented
             skip reason]

Verification: [Phase 5 outcome — divergence at step 4096 as expected,
               or deviation with explanation]

Follow-up: [anything surfaced that's out of scope for this task]
```

Task log entry:

```
## EMU-17
Date: {today}
Status: PASS
Notes: Added scratch-file disk isolation to the harness. Each emulator now
operates on its own copy of the disk image; source remains untouched. Self-
test confirms image md5 unchanged before/after run. Harness output includes
paths to scratch files for offline diff inspection. Phase 5 reproduced
EMU-16's divergence finding at step 4096, confirming no regression in CPU-
state comparison path.
```

Then:

```bash
mv tasks/emu17-task.md tasks/completed/
git add -A
git commit   # paste self-audit message
```

Do **not** push. User reviews and pushes.

**Report** to `tasks/triage/emu17-triage-report.md` if: Phase 1 reveals the reference acquires its disk handle in a way that makes Options A and B both unworkable, Self-test 1 fails in a way you can't resolve, Phase 5 doesn't reproduce EMU-16's finding, or the reference's standalone build is no longer md5-identical.

## Out of scope — do not touch

- **The timer-cadence bug** that EMU-16 found. That's the next task (EMU-18 or whatever), and it's fixed *in our emulator's source*, not in the harness.
- **Other non-determinism sources** beyond disk — `overrides.c` is correct as-is for this task.
- **Keystroke delivery**, timer deterministic-cadence design, or other harness v2 features. Still future work.
- **Any refactor of either emulator** outside of what's strictly needed for disk-path redirection (which ideally is zero, under Option A).
- `editor-api-proposal.md`, latent 0xEA JMP/CALL length bug, silent-exit-on-0:0, register-memory aliasing. All previous out-of-scope items, still out of scope.

If you notice anything worth raising, put it in the Follow-up section of the commit or the Remaining-concerns section of the report.

## Housekeeping

- The untracked `packages/emu86/emu86-dbg` from EMU-14 may still be present. Leave it; don't delete.
- If your design creates scratch files inside the repo, add the minimum `.gitignore` line needed (e.g., `packages/emu86/harness/work/` not `*.scratch`) and flag it in the commit message.
- No scratch files should end up in the commit. Verify with `git status` before `git add`.

## Final note

This task is narrow by design: it's a capability gap in existing tooling, not a new capability or a bug hunt. The failure mode to guard against is *over-engineering*. It's tempting to also redirect stdin/stdout interception, add per-run logs, implement a diff tool, write a wrapper script. All of those are v2. v1 is: scratch files get created, emulators use them, source stays untouched, EMU-16's finding still reproduces.

If you find yourself adding features the brief didn't ask for, stop and reconsider. If you find yourself needing to modify core emulator logic, stop and reconsider. If Option A works cleanly, it's almost certainly the right answer — resist the urge to reach for more elaborate mechanisms.

The harness is infrastructure. The goal is that it becomes boringly reliable, not more interesting.
