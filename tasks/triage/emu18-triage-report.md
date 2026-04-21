# EMU-18 Report
Date: 2026-04-21
Outcome: **PREMISE-FALSIFIED** — paused at end of Phase 1 per brief's Final note

## Summary

The EMU-18 brief's central premise — that the reference drives `int8_asap`
off wall-clock time via `time()` / `ftime()`, and that the harness's
zeroed `time()` override is what prevents the reference timer from
firing — is factually incorrect. The reference sets `int8_asap` purely
from the instruction counter (`inst_counter % KEYBOARD_TIMER_UPDATE_DELAY
== 0`, where `KEYBOARD_TIMER_UPDATE_DELAY = 20000`). `time()`, `ftime()`,
and `localtime()` are only consulted inside the emulator-specific `GET_RTC`
BIOS call (opcode `0x0F 0x01`) to populate a `struct tm` in guest memory
when the guest explicitly asks for it; they do not drive any interrupt
cadence.

Because the two emulators are running on fundamentally different timer
mechanisms — *ours* on `(inst_count & 0x4FFF) == 0`, *the reference* on
`inst_counter % 20000 == 0` — the prescribed virtual-clock design cannot
bring their `int8_asap` firing into lockstep. Virtualising `time()` /
`ftime()` does not change when the reference fires its timer, since its
timer does not consult those functions. Even a perfect implementation of
the brief's design would leave the harness still diverging on `int8_asap`,
just at different instruction numbers.

Per the brief's **Final note** — "If Phase 1 reveals the reference's timer
does something materially different than described … raise it and pause
before implementing" — I stopped at the end of Phase 1, wrote this report,
and made no code changes.

## Evidence

### Phase 1 — what the reference actually does

One single cadence-setting site in `reference/8086tiny.c`, line 709–711:

```c
// Poll timer/keyboard every KEYBOARD_TIMER_UPDATE_DELAY instructions
if (!(++inst_counter % KEYBOARD_TIMER_UPDATE_DELAY))
    int8_asap = 1;
```

with `KEYBOARD_TIMER_UPDATE_DELAY` defined at line 31:

```c
#define KEYBOARD_TIMER_UPDATE_DELAY 20000
```

So the reference fires `int8_asap` exactly on `inst_counter` values
20000, 40000, 60000, …. No time call is involved.

The only other site that can set `int8_asap` is `KEYBOARD_DRIVER`
(line 147), which fires when the guest user presses ESC (0x1B). That's
an interactive path; the harness feeds no keyboard input, so it never
fires in harness mode.

Uses of `time()` / `ftime()` / `localtime()` in the reference:

| Location | What it does | Affects `int8_asap`? |
|---|---|---|
| line 679 `time(&clock_buf)` | Inside `OPCODE 1` (GET_RTC) | No |
| line 680 `ftime(&ms_clock)` | Populates `ms_clock.millitm` → guest memory | No |
| line 681 `localtime(&clock_buf)` | `memcpy` into guest memory at `ES:BX` | No |
| line 682 `ms_clock.millitm` | Written to `mem + SEGREG(…) + 36` | No |

All four are inside a single `case` of the emulator-specific BIOS dispatch
(`OPCODE 48` → `i_data0 == 1`). The call happens only when the guest
executes the 2-byte `0F 01` instruction to explicitly request an RTC
snapshot. The resulting bytes land in guest memory and affect subsequent
guest computation, but do not feed back into when the *host* sets
`int8_asap`.

There are no other wall-clock-driven behaviours in the reference. No
`gettimeofday`, no `clock_gettime`, no `sleep`, no idle throttle, no
RTC-INT-1A path outside `GET_RTC`.

### Why the divergence at step 4096 occurs

Our emulator: `src/emulator/run.c:587`

```c
if ((s->inst_count & 0x4FFF) == 0)
    s->int8_asap = 1;
```

At `inst_count = 4096 = 0x1000`: `0x1000 & 0x4FFF == 0`. Our emulator
sets `int8_asap = 1`.

Reference at the same step: `inst_counter = 4096`, `4096 % 20000 = 4096`
(nonzero). Reference leaves `int8_asap = 0`.

Harness compares → `DIV_INT8_ASAP` set → divergence reported at step 4096.
Confirmed by running the harness (step-limit 5000):

```
======== HARNESS DIVERGENCE ========
Step             : 4096
Ref CS:IP        : F000:02D5
Our CS:IP        : F000:02D5
Ref opcode bytes : 81 FA B8 03 74 EF 83 FA
Our opcode bytes : 81 FA B8 03 74 EF 83 FA
Categories       : 0x00000040
  int8_asap: ref=0 ours=1
```

Both emulators are at the same CS:IP with the same opcode bytes. The
only mismatch is the `int8_asap` flag, set by the host-side cadence
check whose formula differs between ref and hux.

This matches the description already recorded in the EMU-16 task log:

> Root cause is a pre-existing bug in `src/emulator/run.c:587`: the
> timer-tick cadence check is `(s->inst_count & 0x4FFF) == 0`, which
> fires whenever the low 12 bits of inst_count are zero AND bit 14 is
> zero (i.e. at 0x1000, 0x2000, 0x3000, 0x8000, …) rather than every
> 20000 instructions as the adjacent comment claims.

Both the EMU-16 task log and the reference source itself characterise
the issue the same way: it's a cadence mismatch between two
instruction-count-based counters, not a wall-clock-vs-instruction-count
mismatch.

### Why the prescribed design does not fix this

The brief prescribes:

1. Harness maintains `virtual_ns`.
2. `time()` / `ftime()` overrides read from `last_step_ns`.
3. Our emulator's timer consults `platform->get_time_ns()` and fires
   when that crosses `next_tick_ns` (tick interval = 54,945,054 ns ≈
   1/18.2 s).

Steps (2) and (3) are orthogonal to the reference's `int8_asap` firing.
Whatever value `time()` returns, the reference still only sets
`int8_asap` via `++inst_counter % 20000 == 0`. So after implementing
the design, the picture becomes:

- Ref: fires `int8_asap` at inst=20000, 40000, 60000, … (unchanged).
- Hux: fires `int8_asap` when virtual_ns crosses a 54.9 ms boundary.
  Since virtual_ns accumulates measured wall-clock per step, and the
  harness runs near peak CPU speed (no sleeping, no I/O in the fast
  path), one step is typically in the hundreds of ns range. With
  ~300 ns/step, one tick (54.9 ms) ≈ 183,000 steps between fires —
  *very* different from 20000.

The two emulators would still diverge on `int8_asap`, just at different
instructions. The design does not satisfy the correctness criterion
stated in the brief ("'matching the reference' is the correctness
criterion").

### What would actually fix the divergence

Three plausible paths, listed roughly in ascending scope:

1. **Minimal fix** — match the reference's instruction-count cadence
   exactly. Replace `(s->inst_count & 0x4FFF) == 0` with
   `(s->inst_count % 20000) == 0` (or better, extract a named constant
   mirroring `KEYBOARD_TIMER_UPDATE_DELAY`). One line in `run.c`. Takes
   the harness past step 4096; doesn't address the deeper "our timer
   should match real 18.2 Hz hardware" concern.

2. **Partial virtualisation** — implement the virtual-clock mechanism
   in the harness and route *both* `int8_asap` cadence decisions
   through it, overriding the reference's line 710 check via a
   `HARNESS_TIMER_TICK()` macro (in the same style as
   `HARNESS_STEP_BEGIN/END`). This would require modifying
   `reference/8086tiny.c` — which the EMU-18 brief explicitly
   forbids ("you are not modifying this file beyond what already exists
   from EMU-16").

3. **Full virtualisation** — the brief's prescribed design, applied
   bilaterally. Requires the reference to also fire `int8_asap` from
   virtual time rather than from `inst_counter`, which again needs a
   reference-side change.

Only (1) is achievable within the brief's "do not touch the reference"
constraint.

### Performance-metric sub-objective

The brief also asks for a per-step wall-clock measurement printed at
harness shutdown. That part of the design is self-contained and
independently useful — it doesn't depend on the virtual-clock mechanism
being correct to produce a valid metric. It could be carved off into its
own small task without prejudicing the timer discussion.

## Root cause of the brief's premise error

The EMU-16 task log already correctly characterised the bug as "pre-existing
bug in src/emulator/run.c:587 … rather than every 20000 instructions as the
adjacent comment claims." This report takes the same finding at face value,
whereas EMU-18's design question (*"The unresolved design question behind
EMU-18"*) restated the situation as a wall-clock-vs-instruction-count
mismatch. That restatement does not match what the reference actually does.

Speculation: the restatement may have been intended to explain why the
harness's `time()` override needs to be non-zero, on the assumption that
the reference's timer depended on it. But the reference's timer doesn't
depend on `time()`; the override can stay at zero and the harness's
correctness isn't affected (there is a secondary question about `GET_RTC`
consumers — see "Follow-up concerns" below).

## Scope assessment

- **Achievable under the brief's constraints**: option (1) only — match
  the reference's instruction-count cadence exactly. Single-line fix in
  `run.c`. Does not introduce a `get_time_ns` platform hook, does not
  alter `Emu86State`, does not change snapshot format.
- **Not achievable under the brief's constraints**: options (2) and (3),
  because both require modifying `reference/8086tiny.c` beyond
  EMU-16/17's existing macros.
- **Independent of the timer debate**: the performance-metric printout
  (wall-clock per step for both sides, ratio at shutdown). Self-contained.

## Changes made

None. The tree is clean:

```
$ git status
?? tasks/emu18-task.md       # brief, unchanged
?? tasks/triage/emu18-triage-report.md   # this report
```

No edits to `src/`, `harness/`, or `reference/`. No build artifacts
beyond the harness re-build I ran to confirm the step-4096 divergence
reproduces as EMU-17's log described.

## Follow-up concerns

- **The real bug (cadence mismatch).** Needs its own task. Smallest
  scope: change `(s->inst_count & 0x4FFF) == 0` to
  `(s->inst_count % 20000) == 0` in `run.c`. Ideally extract the
  constant into a named define (`EMU86_TIMER_TICK_INSTRUCTIONS`?), pair
  with `KEYBOARD_TIMER_UPDATE_DELAY` in the reference in a comment so
  future readers know they must stay aligned. Add a unit test that
  asserts `int8_asap` rises at exactly `inst_count == 20000` after
  stepping a known stream, and verifies it does NOT rise at any
  earlier point.

- **GET_RTC determinism.** `time()` / `ftime()` / `localtime()` are
  still zeroed by `overrides.c`. If any workload executes the `0F 01`
  `GET_RTC` BIOS call and branches on the returned year/month/day/ms
  fields, the harness's determinism will be preserved (both sides see
  zero), but the emulator's standalone behaviour under `./emu86` will
  differ from a normal boot (guest sees epoch-0 RTC instead of real
  time). Standalone `./emu86` currently uses the same zero-returning
  `get_time_us` (see `harness/harness.c:151`), so the symptom is
  already present. If a future workload cares, make standalone use a
  wall-clock-backed `get_time_us` and guest-side `GET_RTC` would start
  returning current time — separate task.

- **The 8086-hardware-accuracy debate.** A real 8086 PC's timer fires
  at ~18.2 Hz regardless of CPU speed. Neither the reference nor our
  emulator models this accurately today; both fire on
  instruction-count. Moving to a true wall-clock-based timer is a
  valid future goal, but it's a *model change* that would cause our
  emulator to diverge from the reference, not converge with it. It
  should be designed as "future work after we've matched the reference"
  rather than bundled with "fix the harness-detected divergence at
  step 4096."

- **Performance metrics as a standalone deliverable.** The wall-clock
  per-step measurement, ratio printout, and `hux_total_ns` accumulator
  are small, self-contained, and independently useful regardless of
  which timer path we end up on. Could be lifted into their own tiny
  task (half-day scope) without waiting for the timer decision.

- **The `Emu86Platform.get_time_us` vs `get_time_ns` question.** The
  struct currently has `uint64_t (*get_time_us)(void *ctx)` (see
  `src/emulator/platform.h`). If a future task does go down the
  virtual-clock path, converting this to `_ns` is a reasonable unit
  change, but has no bearing on the current divergence.

- **Snapshot format.** The brief asked for `next_tick_ns` to be added
  to snapshot serialisation. That requirement is moot under option
  (1). If a future task introduces a virtual-clock backend, the
  snapshot bump is still needed. Currently no change is necessary.

## Recommended next step

Open a new task, **EMU-19: Fix `int8_asap` cadence to match the reference
(every 20000 instructions)**, scoped similarly to EMU-13 and EMU-15:

1. Change `(s->inst_count & 0x4FFF) == 0` to `(s->inst_count % 20000) == 0`
   in `run.c` case (line ~587). Consider extracting
   `EMU86_TIMER_TICK_INSTRUCTIONS = 20000` into a header shared by
   `run.c` and any test file that asserts cadence.
2. Add a unit test that runs a benign instruction stream for 20000+
   cycles, asserting `int8_asap` is 0 at 19999 and 1 at 20000. Verify
   test fails against unpatched code (revert-and-re-test).
3. Run the harness (no injected step limit). Expect divergence to
   advance past step 4096; observe where it lands next. Any new
   divergence becomes its own follow-up task per EMU-16/17 policy.
4. Run standalone `./emu86 reference/bios test/images/freedos.img`.
   Expect no behavioural regression from the pre-fix cold-boot workload.

If, after EMU-19 lands, the harness still finds the step-4096-style
symptom at some larger N, that's useful information that may motivate
reopening the virtual-clock discussion. Until then, the cheapest
correct fix matches the reference rather than diverging from it under
a different model.

**EMU-18 as currently written is blocked on an incorrect premise and
should not be implemented as-specified.** The design is a reasonable
direction for eventual hardware-accuracy work but does not address the
step-4096 divergence it set out to fix. Recommend closing EMU-18 as
hypothesis-falsified (this report) and opening EMU-19 with the minimal
fix.
