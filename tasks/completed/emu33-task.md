# EMU-33: Fast-compare mode (skip per-step memory scan)

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

Post-EMU-32, the harness is interactive — split-output fifos (EMU-31), keyboard input fifo (EMU-32), full byte-level lockstep verification.

But: the harness runs at around 2000 steps per second. FreeDOS takes ~10-20 minutes to reach the A:\> prompt. Typing commands and waiting for output is painful. Real interactive use (running `debug.com`, assembling small programs) is currently impractical.

Profile: the dominant per-step cost in the harness is the full memory compare. `compare_states` in `harness/harness.c` (around lines 444-461) scans `[0, 0xF0000)` then `[0xF0100, REF_RAM_SIZE)` — effectively 1MB of byte-level comparison every step. At 2000 steps/sec, that's ~2GB/sec of memory bandwidth just for the comparator.

The other per-step checks (registers, flags, sregs, prefixes, trap_flag, int8_asap, inst_count, spkr_en, pit_lobyte_pending, io_ports) are all constant-time — a couple of dozen comparisons, trivial.

### Why full memory compare every step is pure paranoia

Memory divergence matters only insofar as it eventually manifests as a register/flag divergence. If our emulator writes a bad byte and the reference writes a good byte, the bytes differ in memory — but nothing observable has happened yet. Something observable happens when the next instruction that reads that byte puts it into a register or tests it against flags. At that moment, registers differ — and the cheap register comparator catches it.

So full memory compare every step is detecting divergence "before it matters." Not wrong, but paying a ~10x performance penalty for slightly earlier detection.

### When full memory compare IS necessary

Two cases:

1. **REP string instructions.** `REP MOVSB`, `REP STOSB`, `REP LODSB`, `REP CMPSB`, `REP SCASB` (and word variants) can write huge swathes of memory — up to 64KB — within a single "step" from the harness's perspective, because the repeat loop runs inside the emulator. If our REP MOVSB writes one wrong byte out of 1000, and that byte isn't read back for many thousands of subsequent steps, the register comparator won't catch it promptly. After a REP, a full memory compare is prudent.

2. **When regs/flags/sregs/prefixes already diverged.** Before emitting the divergence report, upgrade to full compare so the report includes memory-diff info for diagnostics. The divergence has been caught by the cheap comparators; the full compare is just for the report payload.

Combined policy:
- Normal mode (default): full compare every step. Current behaviour, unchanged.
- Fast mode: cheap-only every step. Upgrade to full compare post-step if (a) REP was active this step, or (b) cheap compare found a divergence.

Environment variable: `HARNESS_FAST_COMPARE=1` enables fast mode. Default off.

### Expected speedup

Memory compare is the dominant cost. Removing it per-step in fast mode should give roughly 5-10x speedup. Exact number depends on how REP-heavy the workload is (since REP instructions force full compare anyway).

After this task, FreeDOS boot in the harness should complete in 2-5 minutes instead of 15-20 minutes. Typing a debug.com command and seeing output should feel near-interactive (still slow by modern standards, but usable).

### What this task does

Split `compare_states` in `harness/harness.c` into:

```c
/* All cheap checks, no memory or io_port scan. */
static uint32_t compare_states_cheap(uint32_t *reg_diff_idx);

/* Full scan (memory + io_ports). Called either per-step (normal mode) or
 * on-demand (fast mode: after REP or after cheap-compare divergence). */
static uint32_t compare_states_full(uint32_t *mem_diff_addr,
                                    uint32_t *io_diff_addr,
                                    uint32_t *reg_diff_idx);
```

The existing `compare_states` becomes `compare_states_full` essentially unchanged. A new `compare_states_cheap` contains the pre-memory-scan portion of the logic.

In the harness step loop (around line 717), replace the single `compare_states(...)` call with:

```c
uint32_t dflags;
if (harness_fast_compare) {
    uint32_t reg_idx_cheap = 0xFFFFFFFF;
    dflags = compare_states_cheap(&reg_idx_cheap);
    reg_idx = reg_idx_cheap;
    
    /* Determine whether to upgrade to full compare */
    int rep_this_step = (our_state->rep_override_en != 0) || (rep_override_en != 0);
    int need_full = (dflags != 0) || rep_this_step;
    
    if (need_full) {
        /* Re-run full compare. This also populates mem_addr and io_addr
         * for the divergence report. */
        dflags = compare_states_full(&mem_addr, &io_addr, &reg_idx);
    }
} else {
    /* Normal mode: always full compare */
    dflags = compare_states_full(&mem_addr, &io_addr, &reg_idx);
}
```

Add the mode flag as a static variable, initialised from env var at harness startup alongside the other HARNESS_* flags:

```c
static int harness_fast_compare = 0;

/* In init, next to maybe_setup_split_output / setup_kbd_input: */
if (getenv("HARNESS_FAST_COMPARE") && !strcmp(getenv("HARNESS_FAST_COMPARE"), "1")) {
    harness_fast_compare = 1;
    fprintf(stderr, "harness: fast compare mode active "
                    "(per-step memory scan skipped; full compare "
                    "on REP and on divergence).\n");
}
```

### Counters for observability

Add two counters so the end-of-run summary can report how often each path fired:

```c
static uint64_t compare_cheap_count = 0;
static uint64_t compare_full_count = 0;
```

Increment in the appropriate branches. Print at harness exit (wherever the step-count summary is printed — search for "steps, CS:IP" in harness.c).

Summary line should look something like:
```
harness: 1500000 cheap compares, 50000 full compares (3.3% full-rate)
```

This tells the user at a glance whether fast mode actually bought them anything for their workload. High full-rate = REP-heavy = smaller speedup. Low full-rate = small REP footprint = big speedup.

### What this task does NOT do

- Does not modify the emulator (`src/emulator/`).
- Does not modify the reference (`reference/`).
- Does not change the divergence report format (mem_addr, io_addr still populated and printed when full compare detects them).
- Does not change any existing comparison logic. `compare_states_full` has identical behaviour to the current `compare_states`.
- Does not add per-step decoder hooks to the harness (REP detection uses the existing `rep_override_en` field on both emulator states; no decode output needed).
- Does not add partial or incremental memory compare. Either full scan or skip entirely. Simplicity over cleverness.

## Your task

### Phase 1 — Read the current comparator

- `harness/harness.c` lines 379-473 — the current `compare_states`.
- `harness/harness.c` line 717 — where it's called in the step loop.
- `harness/harness.c` line 676 — there's a second call site (selftest code); this one uses the result differently, see note below.

The selftest call at line 676 is used for a different purpose (self-consistency after a snapshot restore). It should continue to use `compare_states_full` unconditionally. Only the main step loop's call needs to support fast mode.

### Phase 2 — Split compare_states

Rename the existing `compare_states` to `compare_states_full`. No behaviour change.

Extract a `compare_states_cheap` that contains:
- IP comparison
- General-purpose registers compare (with reg_diff_idx output)
- Segment registers compare
- Flags compare (pack_ref_flags etc.)
- Prefix state compare (including REG_ZERO handling)
- Trap flag, int8_asap, inst_count, spkr_en, pit_lobyte_pending compare
- (NOT memory, NOT io_ports)

Same `flags` bitmask output, just with the DIV_MEM and DIV_IO_PORTS bits never set.

Signature:
```c
static uint32_t compare_states_cheap(uint32_t *reg_diff_idx);
```

No `mem_diff_addr` or `io_diff_addr` outputs — those are meaningless when memory/ports aren't compared. The caller, if it needs them, runs `compare_states_full` afterward.

### Phase 3 — Gate the step-loop call

At the step-loop call site (line 717 area), replace the unconditional `compare_states(...)` call with the gated version shown in the "What this task does" section above.

Preserve the existing divergence handling: if `dflags != 0`, the divergence report is emitted as before, using mem_addr / io_addr / reg_idx values.

### Phase 4 — Env-var setup

In the harness initialisation (alongside `maybe_setup_split_output` and `setup_kbd_input`), add the env-var check and the informational message.

### Phase 5 — Counters

Add `compare_cheap_count` and `compare_full_count`. Increment in appropriate places. Print at end-of-run, next to the existing step-count summary.

### Phase 6 — Verify mode-off behaviour unchanged

```
cd packages/emu86
make harness
./harness/harness reference/bios test/images/freedos.img
```

Expected: identical behaviour to pre-EMU-33. Harness runs at the same speed as before, no regression in step count or divergence detection. (Should reach past 1.6M steps with no divergence, matching prior EMU-30/31/32 runs.)

### Phase 7 — Verify mode-on behaviour

```
HARNESS_FAST_COMPARE=1 ./harness/harness reference/bios test/images/freedos.img
```

Expected:
- Startup prints "harness: fast compare mode active ..." line.
- Significantly faster than normal mode (anecdotally 5-10x).
- Reaches past 1.6M steps with no divergence (same as normal mode).
- End-of-run summary prints the cheap/full compare counts.

Use a time check to quantify speedup. For example, with `HARNESS_STEP_LIMIT=500000`:

```
time HARNESS_STEP_LIMIT=500000 ./harness/harness reference/bios test/images/freedos.img > /dev/null 2>&1
time HARNESS_STEP_LIMIT=500000 HARNESS_FAST_COMPARE=1 ./harness/harness reference/bios test/images/freedos.img > /dev/null 2>&1
```

Real-time comparison. Report the actual numbers in the commit message.

### Phase 8 — Verify divergence detection still works in fast mode

To confirm the "cheap divergence triggers full compare" path, artificially introduce a divergence. Easiest way: run `./harness/harness` with a known-bad step limit (but we have no known bug currently — the emulator is clean past 1.6M). Alternative: modify our emulator in a contrived way to produce a register divergence on step 100, then verify fast mode catches it and reports mem/io info in the divergence block.

If this is awkward to set up, skip the active test and rely on code review: the logic `dflags = compare_states_full(...)` on cheap-divergence is trivially equivalent to calling full compare always. As long as that line is present, divergence detection works.

### Phase 9 — Full test suite

```
make clean
make all
make test
```

Expected: all tests pass. This is harness-only; no emulator changes, no test changes expected.

### Phase 10 — Commit

Commit if:
- Phase 6: no regression in default mode.
- Phase 7: fast mode runs significantly faster (measured), reaches same step count.
- Phase 9: all tests pass.
- `git status` shows only:
  - `harness/harness.c` modified
  - `tasks/emu33-task.md` created
  - No other changes.

Commit message template:

```
EMU-33: Fast-compare mode (skip per-step memory scan)

The harness's per-step full memory compare was the dominant cost
in observed execution time. At ~2000 steps/sec, a 1MB byte-level
scan every step consumes roughly 2GB/sec of memory bandwidth
for the comparator. FreeDOS boot took 15-20 minutes in the
harness vs ~30 seconds on standalone emu86.

Adds HARNESS_FAST_COMPARE=1 mode. When set:
- Cheap checks (regs, flags, sregs, prefixes, trap flag, int8_asap,
  inst_count, spkr_en, pit_lobyte_pending) run every step.
- Full memory+io_ports scan runs only when (a) a REP-prefixed
  instruction executed this step, or (b) cheap compare found a
  divergence (to enrich the report).

Default behaviour (no flag) unchanged — full compare every step.

Measured speedup on FreeDOS boot: {N}x (from {before} to {after}
for {steps} steps).

Harness summary now prints cheap/full compare counts so users can
see what fraction of steps needed full compare.

Scope:
- harness/harness.c: split compare_states into cheap and full;
  gate the main step-loop call on the flag; add counters and
  startup message.

Verification:
- Default mode: no regression; harness advances past 1.6M
  steps with no divergence, same as pre-EMU-33.
- Fast mode: same step count, same lack of divergence, but
  significantly faster.
- make test: all green.

Follow-up:
- EMU-34: disk write sync verification.
- EMU-35: debug.com → HELLO.COM acceptance test.
```

Task log entry:

```
## EMU-33
Date: {today}
Status: PASS
Test results: unchanged (harness-only change)
Harness: adds HARNESS_FAST_COMPARE=1 mode. Skips per-step memory
scan; runs full compare on REP or on divergence. Measured {N}x
speedup on FreeDOS boot.
Notes: Makes interactive harness use practical. Key enabler for
EMU-35 (debug.com acceptance test).
```

Then:
```
mv tasks/emu33-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 11 — Report (on failure)

Triage to `tasks/triage/emu33-triage-report.md` if:
- Phase 6 shows unexpected regression in default mode.
- Phase 7 fast mode causes false divergence (or misses real divergence).
- Phase 7 shows no significant speedup (would suggest memory compare wasn't actually the bottleneck — surprising; worth investigating).
- Phase 9 test suite regresses.

## Out of scope — do not touch

- Emulator source (`src/emulator/`).
- Reference source (`reference/`).
- Makefile.
- Pre-step snapshot (EMU-28).
- Keyboard input (EMU-32).
- Split output (EMU-31).
- Decoder hooks or per-step decoder output.
- Dormant concerns.

## Final note

Failure modes to watch for:

1. **rep_override_en readout timing.** At step end, `rep_override_en` on our side has just been decremented (see run.c's step-loop decrement that EMU-30 moved to top-of-step). So at the moment we read it in the comparator, it reflects "this step had a REP that was decremented to 0" or "this step started with REP still active." Either way, nonzero-post-step means REP was in play during this step. Test both values (our_state->rep_override_en and the reference's rep_override_en) to be robust; if either shows REP was active, force full compare.

2. **Race between fast-mode divergence detection and full-compare output.** When cheap finds a divergence and we upgrade to full, make sure the second comparator call populates mem_addr and io_addr fields. The divergence report reads those fields. If the second call is missed or short-circuited, the report has stale addresses.

3. **Selftest path uses `compare_states_full` unconditionally.** Line 676's selftest call needs to keep its behaviour. When renaming, make sure the selftest call updates its function reference too.

4. **Counter overflow.** uint64_t counters; won't overflow in any practical run duration.

5. **The 3.3% full-rate number in the example is illustrative.** Actual rate depends on REP frequency. For FreeDOS boot, expect something in the 1-10% range.

Once EMU-33 lands, interactive harness use becomes practical. EMU-34 (disk write sync) and EMU-35 (debug.com acceptance) follow naturally.
