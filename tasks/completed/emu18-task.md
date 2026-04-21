# EMU-18: Harness virtual clock; rewire emulator timer to use it

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### The path to here

- EMU-16 built a differential test harness that runs our emulator in lockstep with the reference `8086tiny`, comparing CPU state after every instruction.
- On its first real run, the harness detected a divergence at instruction 4096: our emulator sets `int8_asap = 1` (ready to fire BIOS timer interrupt INT 8) while the reference does not.
- EMU-17 added scratch-file disk isolation to the harness. The divergence-at-4096 finding was reproduced exactly as a regression control.

### The unresolved design question behind EMU-18

The divergence at step 4096 is not a one-line mask-typo bug, despite how EMU-16's summary characterised it. It is the surface symptom of a deeper mismatch:

- **The reference** drives its timer off *wall-clock time*. It calls `time()` / `ftime()`, measures elapsed seconds, and sets `int8_asap` when ~1/18.2 seconds have passed since the last tick. The harness currently neutralises these calls to return 0, so the reference's timer effectively never fires.
- **Our emulator** drives its timer off *instruction count*. Around `run.c:587` is a check of the form `(inst_count & 0x4FFF) == 0` that sets `int8_asap`. This fires at instruction counts 0x1000, 0x2000, ... — not on a sensible cadence, and unrelated to any wall-clock notion.

So on every harness run, our emulator fires the timer flag at instruction 4096 (its first `== 0` hit after boot), the reference does not (its `time()` returns 0 forever), and the harness correctly reports divergence. Fixing the mask alone does not fix the underlying mismatch — we'd still have wall-clock-vs-instruction-count as fundamentally different mechanisms.

The correct fix, which this task implements, is to introduce a **virtual clock** that both emulators consult.

## The design (prescribed — this task implements, does not design)

### Virtual clock

The harness maintains a single monotonic counter called `virtual_ns` measuring virtual elapsed nanoseconds since harness start. Both emulators perceive time through this counter.

Per-step mechanism, driven by the existing `HARNESS_STEP_BEGIN` / `HARNESS_STEP_END` macros in the reference's main loop:

1. `HARNESS_STEP_BEGIN`: record wall-clock start time via `clock_gettime(CLOCK_MONOTONIC, ...)`. Capture the *current* value of `virtual_ns` into a step-local variable `last_step_ns`. During the reference's instruction execution in this step, any `time()` / `ftime()` call returns a value derived from `last_step_ns`, not from `virtual_ns`.
2. The reference executes one instruction.
3. `HARNESS_STEP_END`: record wall-clock end time. Compute `elapsed_ns = end - start`. Add to `virtual_ns`. *This new value is not yet visible to either emulator.*
4. The harness advances our emulator by one instruction, passing `last_step_ns` as the current virtual time it should perceive.
5. After our emulator completes its step, `virtual_ns` becomes the new `last_step_ns` for the next iteration.

The design property this guarantees: **during step N, both emulators perceive the same virtual time**, specifically the virtual time as of the end of step N-1. No cross-step skew is possible.

### Reference-side overrides

Currently `overrides.c` has `time()` / `ftime()` / `localtime()` returning zero. Change them to derive values from `last_step_ns`:

- `time()` returns `last_step_ns / 1_000_000_000` as `time_t` (virtual seconds since harness start).
- `ftime()` fills in time + millitm from `last_step_ns`, timezone and dstflag remain 0.
- `localtime()` may reasonably continue returning a zero-filled struct; the reference uses it for RTC calls that are informational. If analysis shows the reference's behaviour depends materially on `localtime()` returning sensible values, derive a `struct tm` from `last_step_ns` treating it as seconds-since-epoch. Keep this minimal; document what you chose.

### Our emulator's timer backend

Our emulator needs two timer backends:

- **Wall-clock backend** for standalone use (running `./emu86 reference/bios test/images/freedos.img` directly, outside the harness). Uses `clock_gettime` or equivalent. Fires `int8_asap` when ~1/18.2 seconds of wall-clock time has elapsed since the last tick.
- **Virtual-clock backend** for harness use. Fires `int8_asap` when the virtual time crosses the next tick threshold.

Selected at initialisation. The existing `Emu86Platform` abstraction is the natural home for this — add a function pointer like `uint64_t (*get_time_ns)(void *ctx)` that each backend implements differently. The emulator consults this pointer instead of consulting `inst_count`. The buggy `(inst_count & 0x4FFF) == 0` check goes away entirely.

The tick threshold is 54,945,054 nanoseconds (= 1/18.2 seconds, nearest integer). Consistency matters more than precision — whatever value both emulators use as "the tick interval," they must use the same one.

### Performance metrics (informational only)

While the harness is already measuring wall-clock time for `virtual_ns`, also measure how long *our* emulator takes per step. This is pure instrumentation — neither emulator consults it, but it's printed at harness shutdown as a comparative metric.

At shutdown, print (format suggested, adjust freely):

```
Harness run statistics:
  Total instructions:    N
  Virtual time elapsed:  T ms
  Reference wall time:   Tr ms (avg Tr/N ns/instr)
  Hux wall time:         Th ms (avg Th/N ns/instr)
  Hux/Ref ratio:         R.RR
```

This is the first building block of the performance-stop-loss discussion we've had. Once it exists, future changes that impact performance become visible.

## Required reading before you touch code

1. **`packages/emu86/harness/harness.c`** — the existing harness main loop and the macro implementations.
2. **`packages/emu86/harness/overrides.c`** — the deterministic stubs. You will modify `time`, `ftime`, and possibly `localtime`.
3. **`packages/emu86/reference/8086tiny.c`** — specifically how it uses `time()` / `ftime()` to drive the timer. Look for the lines that set `int8_asap`. Understand the cadence logic. You are not modifying this file beyond EMU-16/17's existing macros; you are changing what `time()` *returns* via overrides.
4. **`packages/emu86/src/emulator/run.c`** at approximately line 587 — the current timer cadence logic. This is what gets rewired.
5. **`packages/emu86/src/hosts/linux/platform_linux.c`** and **`platform_linux.h`** — the current Linux platform layer. The new timer backend interface is added here (or alongside it).
6. **`packages/emu86/src/emulator/platform.h`** (or wherever the `Emu86Platform` struct is defined) — the place where the timer function pointer gets added.

## Your task

### Phase 1 — Investigate the reference's timer logic

Read the reference's timer code. Document in the commit message:

- What conditions cause the reference to set `int8_asap`?
- What's the tick cadence in terms of wall-clock time?
- Does the reference use `time()` at 1-second granularity, `ftime()` at millisecond granularity, or some combination?
- Are there any other wall-clock-driven behaviours (RTC-INT-1A, keystroke timing, anything else) that we need to consider?

This is the ground truth for what "match the reference" means. Don't guess.

### Phase 2 — Design the platform-timer interface

Add a timer function pointer to `Emu86Platform`. The specific signature is up to you, but it must:

- Return virtual-nanoseconds-since-some-reference-point (monotonic, non-decreasing).
- Be consultable from our emulator's run loop without involving `inst_count`.
- Admit both wall-clock-backed and virtual-clock-backed implementations.

Implement both backends:

- `platform_linux_wallclock` — the default for standalone runs. Uses `CLOCK_MONOTONIC`.
- `platform_linux_virtualclock` — for harness use. Reads from a variable the harness updates.

The harness's initialisation of our emulator should wire up the virtual-clock backend. Standalone `./emu86` uses the wall-clock backend (default).

### Phase 3 — Rewire our emulator's timer

In `run.c`, remove the `(inst_count & 0x4FFF) == 0` check and replace with a threshold check against the platform's virtual time. Something like:

```c
uint64_t now_ns = platform->get_time_ns(platform->ctx);
if (now_ns >= state->next_tick_ns) {
    int8_asap = 1;
    state->next_tick_ns += TICK_INTERVAL_NS;
}
```

`next_tick_ns` becomes part of `Emu86State`. It's set at init to `TICK_INTERVAL_NS` (so the first tick fires after one tick-interval has elapsed, not immediately).

Add `next_tick_ns` to the snapshot serialisation — it's part of persistent state. Handle backward compatibility per existing snapshot-versioning conventions, or document if no such convention exists and you had to introduce one.

### Phase 4 — Harness virtual clock integration

Implement the `virtual_ns` accumulator and `last_step_ns` snapshot in `harness.c`. Wire the `HARNESS_STEP_BEGIN` / `HARNESS_STEP_END` macros to measure real time and accumulate.

Update `overrides.c` so `time()` / `ftime()` / `localtime()` derive from `last_step_ns`. This requires `last_step_ns` to be accessible from overrides.c — a file-scope global in harness.c with an `extern` declaration in overrides.c is fine.

Add the performance-metric accumulator (`hux_total_ns`) and print at shutdown.

### Phase 5 — Self-tests

Before the full harness run, verify:

**Self-test 1 — Time monotonicity.** A short harness run of, say, 1000 steps should produce a `last_step_ns` sequence that is monotonically non-decreasing on every step. Assert this in the harness at step boundaries or in a post-run check.

**Self-test 2 — Tick consistency.** After N steps, the number of timer interrupts fired by *both* emulators should be equal. (They may fire on the same step, or on adjacent steps depending on exactly when each emulator polls — document what the acceptable window is.) A useful implementation: count `int8_asap` rising edges on each side, verify they match within some small delta (or ideally exactly) at harness shutdown.

**Self-test 3 — Regression control.** The harness should still work for the cold-boot workload. Run it against FreeDOS and observe what happens. This will be either:

- (a) No divergence at step 4096 anymore (the timer fix resolved it), and lockstep continues until some later point.
- (b) A divergence still at step 4096, but for a different reason — investigate.
- (c) No divergence ever, clean run to step limit — the timer fix resolved it and nothing else differs for the workload's duration.

Any of (a), (b), (c) is informative. (a) and (c) are green; (b) requires diagnosis.

### Phase 6 — Full run against FreeDOS

Run the harness without any injected step limit. Let it run as long as it runs. Report:

- How far it got (instruction count)
- Whether it halted via divergence (with report), step limit, or natural end
- If divergence, what diverged and at what instruction
- Performance metrics at the end

Any divergence found is a follow-up task, not an in-scope fix.

### Phase 7 — Commit (on success) or report (on failure)

**Commit** if: Phase 5 self-tests pass, Phase 6 produces a coherent result (even if it's a new divergence report), and standalone `./emu86` still works for the cold-boot workload (using the wall-clock backend, not the harness virtual-clock backend).

Do the standalone check explicitly:

```
./emu86 reference/bios test/images/freedos.img
```

Expected: reaches the FreeDOS kernel banner and then the familiar divide-by-zero loop, same as before EMU-18 (but now driven by the wall-clock timer backend). If standalone behaviour regresses in any way, do not commit — the two backends need to both work, and regressing standalone is a scope violation.

Commit message structure:

```
EMU-18: Harness virtual clock; platform-timer rewiring

Reference timer analysis (Phase 1): [one paragraph on what drives
                                     int8_asap in the reference]

Platform timer interface (Phase 2): [signature, how backends are
                                     selected, where they live]

Emulator rewiring (Phase 3): [what changed in run.c, how next_tick_ns
                              fits into Emu86State, snapshot handling]

Harness integration (Phase 4): [virtual_ns mechanism, overrides.c
                                changes, perf metric output]

Self-test results (Phase 5): [outcomes of all three]

Full harness run (Phase 6): [instruction count reached, divergence (if
                             any), performance numbers]

Standalone regression check (Phase 7): [result]

Follow-up: [any divergence found by the harness becomes the next task;
            any design limitations; anything out of scope]
```

Task log entry:

```
## EMU-18
Date: {today}
Status: PASS
Notes: Introduced virtual-clock mechanism in harness; rewired our emulator
timer from instruction-count-based to wall-clock/virtual-clock-based (two
backends, selected at init). Reference and hux now share a virtual clock
during harness runs. Performance metrics now reported at harness shutdown.
Harness reached step N, {divergence at M / clean to step limit / etc}.
Standalone ./emu86 boot still works: {result}.
```

Then:

```bash
mv tasks/emu18-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

**Report** to `tasks/triage/emu18-triage-report.md` if: Phase 1 reveals the reference's timer mechanism is incompatible with the proposed design, self-tests fail in a way you can't resolve, standalone `./emu86` regresses, snapshot format changes can't be made backward-compatible without a principled decision, or anything else prevents a clean commit.

## Out of scope — do not touch

- **Fixing any new divergence the harness finds after the timer rewiring.** If the harness runs further and hits a new divergence, that's a finding, not a fix-in-this-task.
- **ELKs support, keystroke handling, multi-image workloads, or any other harness v2 features.** Still future work.
- **Snapshot format migration tooling.** If your snapshot-version bump is backward-incompatible, document the migration need as follow-up rather than building a migration tool in this task.
- **Refactoring the reference.** You are not modifying `reference/8086tiny.c` beyond what already exists from EMU-16.
- `editor-api-proposal.md`, latent 0xEA JMP/CALL length bug, silent-exit-on-0:0, register-memory aliasing. All previous out-of-scope items, still out of scope.

## Housekeeping

- Scratch files from harness runs live in `/tmp/emu86-harness/` per EMU-17. Don't change this.
- `packages/emu86/emu86-dbg` may still be present from EMU-14. Leave it.
- Any new scratch or debug binaries you create: check `.gitignore` covers them, extend with minimum-glob scope if needed, flag in commit message.

## Final note

This is a design-heavy task dressed as an implementation task. The design is prescribed (above), but the *correctness* of that design depends on Phase 1 findings matching assumptions. If Phase 1 reveals the reference's timer does something materially different than described — say, it uses millisecond-resolution `ftime()` in a way that matters, or drives interrupt behaviour from multiple clock sources — raise it and pause before implementing. "Matching the reference" is the correctness criterion; guessing wrong about what it does defeats the purpose.

The failure mode for this task is *drift between design and implementation* — writing code that kinda-sorta implements the design but takes shortcuts that produce surface-level working behaviour without the deep lockstep property. The self-tests in Phase 5 are specifically constructed to catch this: time monotonicity and tick-count consistency are both properties that are trivial to pass if the design is faithfully implemented, and very easy to fail if it's not.

Also: this task adds a capability to the harness (validated timer behaviour) and simultaneously uses that capability (by rewiring our emulator's timer to the new mechanism). That coupling is intentional — testing the capability in isolation would mean committing a tool that nobody uses yet, and tools nobody uses rot. But it also means the commit is *larger* than a single narrow fix. Don't be alarmed by that; the cohesion is the point.
