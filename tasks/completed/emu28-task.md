# EMU-28: Harness pre-step state snapshot with rotating buffers

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

Every divergence the harness has reported — EMU-19, 20, 21, 22, 26, 27 — has followed the same awkward pattern:

```
Step N diverged.
Reported CS:IP = X:Y    (this is the POST-step CS:IP — the instruction that diverged is upstream)
Opcode bytes at X:Y     (these are the bytes we'll execute NEXT, not the ones that diverged)
```

To identify the actual divergent instruction, we've been running the harness a second time with `HARNESS_STEP_LIMIT=N-1` to see the CS:IP before step N fired. This works but:

1. It's a manual extra step every investigation.
2. It doesn't give us pre-step register state — only the CS:IP at step-limit-exit.
3. The two-run approach is fragile: if the harness's execution is at all non-deterministic (as it silently was pre-EMU-25), the second run can show different state.

This task adds pre-step state capture to the harness so divergence reports include both the pre-step state (state going into the diverging instruction) and the post-step state (state after, including the divergent values).

### The buffer-rotation design

At each step boundary, we need pre-step state of the *about-to-run* step and post-step state of the *just-finished* step. These are the same state: the end-state of step N equals the start-state of step N+1.

So rather than take two separate snapshots per step, we maintain two small buffers per side and rotate which one is "pre" and which is "next" each step:

- At end of clean step N: buffer A holds state-after-N (= state-before-N+1). Pointer `pre` points at A.
- At end of clean step N+1: buffer B holds state-after-N+1 (= state-before-N+2). Swap pointers: `pre` now points at B; buffer A is scratch (will be overwritten in step N+2).
- If step N+1 diverges: `pre` still points at A (holding state-before-N+1). The live reference/our state is state-after-N+1. Report shows both.

This costs one pointer swap and one small memcpy per step. The buffers themselves are tiny (~50 bytes each) — just registers, segment registers, IP, flags, a few status bytes, and the 8 opcode bytes at the CS:IP we're about to execute. NOT memory, NOT io_ports, NOT disk state. Those are available in the reference's live state when needed and the divergence report already handles memory/io via the comparator.

### What this task does

1. Add a small `StepSnapshot` struct (registers, sregs, ip, flags, int8_asap, seg_override_en, rep_override_en, trap_flag, opcode_bytes[8], inst_count).
2. Allocate four buffers total (two per side: ref_a/ref_b, our_a/our_b) plus two pointers per side (pre and next).
3. Initialise all four buffers to zero at harness startup. (The "pre" pointers will point at zero-filled buffers for step 1, but that doesn't matter — the harness never diverges at step 1.)
4. At the end of each harness step (after execution and after comparator), populate the `next_*` buffers with the current live state. Then swap: `pre ↔ next`.
5. Enhance `report_divergence` to print the pre-step state using the `pre_*` buffers alongside the post-step state it already prints.

### What this task does NOT do

- No emulator changes (nothing under `src/emulator/`).
- No Makefile changes.
- No changes to what the comparator compares or how divergences are detected. Same set of flags, same semantics. Only the reporting gets richer.
- No changes to the existing step-limit instrumentation — it already shows CS:IP and opcode bytes on clean step-limit exit, and that's fine to keep as is.
- No changes to the existing heartbeat logging (EMU-24).
- No changes to the existing EMU-22 REG_ZERO sentinel handling in the comparator.
- No changes to memory comparison or memory divergence reporting.

## Your task

### Phase 1 — Read the existing harness code

Key files to read before touching anything:

- `packages/emu86/harness/harness.c` — everything; especially `compare_states` (around line 379), `report_divergence` (grep for it), `harness_step_end` (around line 621), and the snapshot-from-ref code at initialisation (around lines 226-253).
- `packages/emu86/src/emulator/state.h` — the `Emu86State` definition so you know the field names.

Check the reference's side:
- `packages/emu86/reference/8086tiny.c` — confirm the `HARNESS_STEP_BEGIN()` macro is called near the top of the main loop (before the instruction decode), and `HARNESS_STEP_END()` is called near the bottom (after timer service). Don't modify this file.

### Phase 2 — Design the snapshot struct

Create a `StepSnapshot` type in `harness.c` near the top (or in a new small header `harness/snapshot.h` if you prefer — but a type used only inside harness.c belongs in harness.c). Fields:

```c
typedef struct {
    uint16_t regs[8];        /* AX, CX, DX, BX, SP, BP, SI, DI — using OUR indexing */
    uint16_t sregs[4];       /* ES, CS, SS, DS — using OUR indexing */
    uint16_t ip;
    uint16_t flags;
    uint8_t  int8_asap;
    uint8_t  seg_override_en;
    uint8_t  rep_override_en;
    uint8_t  trap_flag;
    uint8_t  seg_override;   /* OUR sreg index (0-3), or 0xFF if none */
    uint8_t  rep_mode;
    uint8_t  opcode_bytes[8];
    uint32_t inst_count;
} StepSnapshot;
```

Allocate two buffers per side:

```c
static StepSnapshot snap_ref_a, snap_ref_b;
static StepSnapshot snap_our_a, snap_our_b;
static StepSnapshot *pre_ref = &snap_ref_a;
static StepSnapshot *next_ref = &snap_ref_b;
static StepSnapshot *pre_our = &snap_our_a;
static StepSnapshot *next_our = &snap_our_b;
```

All four buffers are zero-initialised by virtue of being statics.

### Phase 3 — Populate snapshot buffers

Add two functions:

```c
static void populate_ref_snapshot(StepSnapshot *s)
{
    /* pull from reference's live state: regs16, reg_ip, seg_override, etc. */
    /* sregs: s->sregs[OUR_INDEX_i] = regs16[REF_REG_ES + OUR_INDEX_i]; */
    /* opcode_bytes: read 8 bytes from linear(CS:IP) in mem[] */
}

static void populate_our_snapshot(StepSnapshot *s)
{
    /* copy the small fields from our_state — registers, sregs, ip, flags, etc. */
    /* opcode_bytes: read 8 bytes from linear(our CS:IP) in our_state->mem[] */
}
```

Be careful with segment-register indexing. The reference stores as ES, CS, SS, DS (indices 8-11 in regs16 with REF_REG_ES=8). Our emulator uses indices 0-3 for ES, CS, SS, DS. Use the existing `REF_REG_ES` constant and `ref_to_ours_sreg` helper where appropriate.

For the opcode_bytes field, read 8 bytes from the linear address `(CS << 4) + IP`, being careful with the REGS_BASE hole. Memory access shouldn't cross the 0xF0000..0xF0100 region in normal operation (the instructions we're tracing are in FreeDOS code around 1FE0:xxxx or BIOS code around F000:xxxx), but clip gracefully if somehow it does.

**Call both populate functions at the end of every clean harness step**, after execution and comparison. Then swap the pre/next pointers for next step:

```c
populate_ref_snapshot(next_ref);
populate_our_snapshot(next_our);
{
    StepSnapshot *tmp = pre_ref; pre_ref = next_ref; next_ref = tmp;
    tmp = pre_our; pre_our = next_our; next_our = tmp;
}
```

The swap makes the *new* populated buffer become the "pre" for next step. The old pre-buffer becomes the next scratch.

Do NOT populate the snapshot on the divergence path. When divergence is detected, the pre_* buffers still hold the previous clean step's end state — which is exactly the pre-step state for the diverging step.

### Phase 4 — Enhance divergence reporting

Find `report_divergence` in `harness.c`. Extend it to print pre-step state *before* the existing post-step divergence details.

New output shape:

```
======== HARNESS DIVERGENCE ========
Step             : {N}

-- Pre-step state (both sides agree, from end of step N-1) --
CS:IP            : {pre_ref_cs:04X}:{pre_ref_ip:04X}
Opcode bytes     : {8 bytes}
Registers        : AX={} CX={} DX={} BX={} SP={} BP={} SI={} DI={}
Segment regs     : ES={} CS={} SS={} DS={}
FLAGS            : {hex}
Status           : int8_asap={} seg_ovr_en={} rep_ovr_en={} TF={}

-- Post-step divergence (after step N executed) --
Ref CS:IP        : F000:0332
Our CS:IP        : F000:067D
Ref opcode bytes : 1E 06 50 53 55 0E 1F BB
Our opcode bytes : 50 53 52 55 06 51 57 1E
Categories       : 0x00000103
  IP differs ...
  GP registers ...
  ...
====================================
```

Two notes:
- The existing post-step divergence text stays intact. We're adding a pre-step block before it, not replacing.
- The pre-step state is "both sides agree" because if they didn't, a prior step would have diverged. Print it once, not twice. Save vertical space.

If the divergence report spans many lines already (memory diffs etc.), consider putting the pre-step block first so it's visible without scrolling — it's usually the most useful piece for diagnosis.

### Phase 5 — Verify via live divergence

Rebuild and run the harness as-is. It will diverge at step 70,424 (the OF-on-something bug waiting for EMU-29).

```
cd packages/emu86
touch harness/harness.c && make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -40
```

Expected: the divergence report now includes a pre-step block showing:

- CS:IP of the instruction that diverged (will be something like 1FE0:7C?? — the instruction preceding 1FE0:7D1D post-step)
- 8 opcode bytes starting at that CS:IP
- Full register state going into that instruction
- Flag state before the instruction

The pre-step CS:IP should be *earlier* than the post-step CS:IP (or different in a way that reflects a branch/interrupt).

Verify the pre-step state looks correct by cross-referencing with `HARNESS_STEP_LIMIT=70423 ./harness/harness ...` — they should show the same CS:IP. If they don't, something in the snapshot capture is wrong.

### Phase 6 — Self-test coverage

Run the existing harness self-test:

```
HARNESS_SELFTEST=1 HARNESS_STEP_LIMIT=100 ./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -5
```

Expected: `SELFTEST PASS — 100 steps, zero divergences after snapshot-compare each step.` The self-test now also exercises the snapshot-population path across 100 steps, so this is a lightweight check that the new code doesn't crash.

Also run the injection self-test:

```
HARNESS_INJECT_AT=50 ./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -30
```

This forces a divergence at step 50 (via the existing AX-XOR mechanism, if the inject-at variable is still supported — grep for `harness_inject_at` to confirm). The report should show pre-step state for step 50, which is still in the BIOS port-scan loop — CS:IP around F000:02D5 or F000:02EA, registers whatever they are at that point in boot.

If the inject mechanism isn't wired in the current harness, skip this sub-phase and rely on Phase 5's natural divergence test only.

### Phase 7 — Full build and test suite

```
cd packages/emu86
make clean
make all        # or whatever the "build everything" target is; check the Makefile
make test
```

Expected: all existing tests pass (harness isn't covered by unit tests, but the build succeeds and unit tests aren't broken by transitive changes).

### Phase 8 — Commit

Commit if:
- Phase 5 shows pre-step state in divergence report with correct CS:IP and plausible register values.
- Phase 6 self-test passes.
- Phase 7 full test suite is green.
- `git status` shows only:
  - `harness/harness.c` modified
  - Possibly a new `harness/snapshot.h` if you chose to split (not required)
  - `tasks/emu28-task.md` created
  - No emulator changes, no Makefile changes, no test changes
  - No binaries

Commit message:

```
EMU-28: Harness pre-step state snapshot with rotating buffers

Every divergence report so far has had the "reported CS:IP is the
post-step address, not the divergent instruction" misdirection. We
have been running the harness twice for each investigation — once
to see the divergence, once with HARNESS_STEP_LIMIT=N-1 to see the
pre-step CS:IP. Awkward and error-prone.

This task adds a small pre-step state snapshot to the harness so
divergence reports include both pre-step and post-step state in a
single run.

Design: rotating buffer pair. At end of clean step N, populate the
"next" buffer with current live state. Swap pointers so "next"
becomes "pre" for step N+1. If step N+1 diverges, the "pre" buffer
still holds state-before-N+1 (populated at end of step N), which
is exactly what the divergence report needs.

Costs per step: one small memcpy (~50 bytes per side) and one
pointer swap. Negligible. Divergence report now includes pre-step
registers, flags, CS:IP, and 8 opcode bytes at that CS:IP —
enough to identify the divergent instruction and its inputs.

Scope:
- harness/harness.c only
- No emulator changes
- No Makefile changes
- No test file changes

Verification:
- Natural divergence at step 70,424 now reports pre-step CS:IP
  identifying the divergent instruction
- Self-test passes (HARNESS_SELFTEST=1 HARNESS_STEP_LIMIT=100)
- Full make test passes

Follow-up:
- EMU-29: the OF-at-step-70,424 investigation, using EMU-28's
  richer diagnostic output.
```

Task log entry:

```
## EMU-28
Date: {today}
Status: PASS
Test results: unchanged (harness infrastructure only)
Harness: divergence reports now include pre-step state via
rotating-buffer snapshots. Divergence at step 70,424 still
exists (EMU-29 territory); its pre-step CS:IP is now visible
directly in the report.
Notes: User suggested buffer-rotation optimisation — small
buffers per side, pointer swap at clean-step boundary, so pre
state is free at divergence time.
```

Then:

```
mv tasks/emu28-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 9 — Report (on failure)

Triage to `tasks/triage/emu28-triage-report.md` if:
- Phase 5's pre-step CS:IP doesn't match `HARNESS_STEP_LIMIT=70423` cross-reference (snapshot capture is wrong).
- Phase 6 self-test fails (new code regressed something).
- Phase 7 full test fails in unexpected ways.
- Design tradeoffs emerge that weren't anticipated (e.g. some case where the rotating-buffer pattern doesn't work cleanly).

## Out of scope — do not touch

- Emulator code under `src/emulator/`.
- The step-66,392 or step-69,489 fixes (those are already committed as EMU-26 and EMU-27).
- The step-70,424 OF divergence (that's EMU-29 — this task makes diagnosing it easier, but doesn't fix it).
- EMU-25's patch-reference.js or Makefile rules.
- The existing heartbeat logging.
- Any test file changes — this task doesn't need unit tests; the acceptance test is the natural divergence at step 70,424.
- Memory comparison logic in the comparator.
- REG_ZERO sentinel handling from EMU-22.
- Dormant concerns: editor-api-proposal, 0xEA length bug, 0xC0/0xC1, silent-exit-on-0:0, register-memory aliasing, FreeDOS divide-by-zero, Makefile header-dependency quirk.

## Final note

Two failure modes to watch for:

1. **Segment register indexing mismatch between ref and ours.** The reference stores ES/CS/SS/DS at regs16[8-11] with REF_REG_ES=8 as the base. Our emulator uses sregs[0-3] with SREG_ES=0. When populating the snapshot, use the *ours-style* indexing consistently across both snapshots — easier to compare visually. The existing code has helpers for this conversion; use them.

2. **Opcode bytes at linear(CS:IP) — guard the memory bounds.** Reading 8 bytes starting at the linear address should clip to available memory (nominal 1MB). If CS:IP wraps around the top of memory, reading past the end would be a buffer overflow. Simplest guard: `for (int i = 0; i < 8 && (linear+i) < REF_RAM_SIZE; i++) ...`. In practice this never triggers for normal boot, but defensive.

The rotating-buffer pattern is deliberately simple. Don't over-engineer. No dynamic allocation, no reference counting, no "three buffers for extra safety" — two buffers per side, one swap per step, that's all.
