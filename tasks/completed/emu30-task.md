# EMU-30: Decrement seg_override_en/rep_override_en before decode, not after

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

With EMU-29 landed, the harness now advances to step 1,103,526 before diverging. The divergence has a clean characterisation (thanks to EMU-28's pre-step snapshot):

```
Pre-step CS:IP   : 9001:CBA4
Opcode bytes     : 8B 46 06 ...         (MOV AX, [BP+0x06])
Registers        : AX=0000 BX=0516 SP=2334 BP=2334 SI=FFFE DI=FFFE
Segment regs     : ES=00D8 CS=9001 SS=9DCA DS=9DCA
FLAGS            : 0244
Status           : int8_asap=0 seg_ovr_en=1 rep_ovr_en=0 TF=0

Post-step divergence:
  AX: ref=0516 ours=8002
```

Both sides: same registers, same segment registers, same pre-step flags, same `seg_override_en=1`. They execute the same `MOV AX, [BP+0x06]` instruction. But they read different values from memory: ref gets 0x0516 (which happens to equal BX, suggesting BX was stored there recently), ours gets 0x8002.

Different values from "the same" memory access means the two sides are computing different effective addresses. Same BP, same displacement — so the *segment* must differ.

### Root cause: decrement ordering in the step loop

In `src/emulator/run.c`, the main step loop does:

```c
/* 4. Decode instruction at CS:IP */
decode_instruction(s, t, &d);           // line 566

/* 5. Decrement prefix override counters */
if (s->seg_override_en)                 // line 569
    s->seg_override_en--;
if (s->rep_override_en)                 // line 571
    s->rep_override_en--;

/* 6. Execute */
execute_instruction(s, p, t, &d);       // line 575
```

Decode happens first (line 566), which calls into `decode.h` and computes the effective address using the current `seg_override_en`. Then the decrement runs. Then execute uses the already-computed address.

The reference 8086tiny does it the other way round — decrements at top-of-loop, BEFORE decode. So the reference's decode sees the decremented value.

### Why this matters for EMU-22's LEA idiom

EMU-22 intentionally set `seg_override_en=1` at the end of our LEA handler to mirror a reference quirk (see `run.c:233-239` and `docs/notes/8086tiny-quirks.md`). The comment there reads:

> LEA idiom: reference sets seg_override_en=1 as a side effect of its "compute offset via segment-override machinery" trick. We don't use that trick (we reconstruct the offset directly), but we set the flag anyway to keep internal state aligned for harness lockstep. Decremented to 0 before next instruction executes, so no CPU-visible effect.

The "no CPU-visible effect" claim is wrong. Our decrement runs *after* decode of the next instruction, so the next instruction's decode reads `seg_override_en=1` and applies the segment override to its effective-address computation. If that next instruction is a memory-accessing instruction (like `MOV AX, [BP+6]`), it reads from the override segment instead of its default.

On the reference side: LEA sets seg_override_en=1. Next instruction's top-of-loop decrement reduces it to 0 *before* decode. Decode sees 0, no override applied. Default segment used.

Hence the divergence: our MOV reads from override-segment:[BP+6] = 0x8002; reference's MOV reads from SS:[BP+6] = 0x0516.

### What `seg_override` was

The pre-step snapshot format doesn't currently include `seg_override` (the index into sregs). We don't know from the output which segment was active as the override. But the memory contents suggest it wasn't SS (= 0x9DCA); it was something else — maybe ES (= 0x00D8) or CS (= 0x9001). Not critical for the fix, but worth noting that this gap in diagnostic output existed.

### Structural shape of the fix

Move the two decrement blocks from after-decode (lines 568-572) to before-decode (before line 566, after the interrupt-service block around line 556). The decode then sees the current step's post-decrement value, matching the reference's behaviour.

This is a two-line move. Formulas unchanged. Initial values unchanged (LEA still sets to 1, segment prefix still sets to 2, REP prefix at line 369-370 still re-increments).

### Why the fix works across all relevant cases

**Case A: Normal segment-override prefix (`ES: MOV [BX], AX`):**
- Step N (prefix 0x26): enters with seg_ovr_en=0. Top-of-loop decrement: still 0. Decode prefix, execute, which sets seg_ovr_en=2. End: 2.
- Step N+1 (MOV [BX], AX): enters with 2. Top-of-loop decrement: 1. Decode (override applies, rm_addr uses ES). Execute writes to ES:[BX]. End: 1.
- Step N+2 (next instr): enters with 1. Top-of-loop decrement: 0. Decode (no override). End: 0.

Correct. Override applies only to the instruction after the prefix.

**Case B: LEA side-effect (today's bug):**
- Step N (LEA): enters with 0. Decrement: 0. Decode+execute, sets seg_ovr_en=1 at end. End: 1.
- Step N+1 (next instr): enters with 1. Top-of-loop decrement: 0. Decode (no override — correct!). End: 0.

Correct. LEA's side-effect doesn't leak into the next instruction's decode.

**Case C: REP-prefixed instruction:**
- Step N (REP prefix F2/F3): enters with rep_override_en=0. Decrement: 0. Decode+execute, sets rep_override_en=2. If preceded by segment override, there's special handling at lines 369-370 to re-increment seg_override_en so it stays alive.
- Step N+1 (string instruction): enters with rep_override_en=2. Top-of-loop decrement: 1. Decode+execute — REP semantics, possibly repeating. End: 1.
- Step N+2 (next instr): enters with 1. Decrement: 0. Decode (no REP). End: 0.

Correct. Behaviour unchanged by the move.

### What this task does

Move the two decrement blocks (lines 568-572) to execute *before* `decode_instruction` (line 566). Keep the blocks together. Update the surrounding comment block from "Decrement prefix override counters" to something that reflects "decrement at top-of-step (pre-decode), so decode sees the correct value for this step."

Update the misleading comment on LEA (run.c:233-239) to remove the "no CPU-visible effect" claim and describe the actual mechanism: the decrement-before-decode at top of next step reduces seg_override_en to 0 before the next instruction's decode, preventing override leakage.

Optionally (if time permits): extend the EMU-28 pre-step snapshot in `harness.c` to also print `seg_override` (the sreg index, 0-3 or 0xFF for none). Small additive change; helps diagnose future segment-related divergences without investigation.

### What this task does NOT do

- Does not change the LEA idiom itself (the seg_override_en=1 setting stays — it's still needed to match the reference's post-LEA state at harness-compare time).
- Does not change `exec_segment_override` (still sets 2).
- Does not change `exec_rep_prefix` (still sets 2).
- Does not change the REP-prefix re-increment at lines 369-370.
- No new opcode implementations.
- No Makefile changes.
- Does not touch the comparator's REG_ZERO handling.

## Your task

### Phase 1 — Confirm the divergence and current baseline

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img
```

Expected: advances to step 1,103,526 then diverges with the pre-step and post-step blocks described above. AX differs (ref=0516, ours=8002), everything else matches.

Note: do NOT pipe to `tail`. The harness takes a while to reach step 1.1M, and piping to tail delays output until completion. Just let it run; scroll back to find the divergence block.

### Phase 2 — Understand the current ordering

Read `src/emulator/run.c` around lines 560-600. Identify:

- The decode call (line 566).
- The two decrement blocks (lines 568-572).
- The execute call (line 575).

Also read:
- `src/emulator/decode.h:340-358` — where `seg_override_en` is consulted during effective-address computation.
- `src/emulator/run.c:220-244` — LEA handler where seg_override_en=1 is set.
- `src/emulator/run.c:367-371` — REP prefix re-increment.
- `src/emulator/opcodes/flags_io.h:93-100` — `exec_segment_override` and `exec_rep_prefix`.

Confirm your mental model matches the analysis in the context section.

### Phase 3 — Move the decrement

In `run.c`, move the two decrement blocks from lines 568-572 to just before the decode call at line 566. The new structure should look like:

```c
        /* 3. Check HLT state */
        if (s->halted) { ... }

        /* 4. Decrement prefix override counters at top of step.
         * The reference 8086tiny decrements these before decode, so
         * that decode sees the post-decrement value. LEA in particular
         * sets seg_override_en=1 as a side effect (see run.c around
         * line 239); without this ordering, that 1 would leak into
         * the next instruction's decode and wrongly apply override. */
        if (s->seg_override_en)
            s->seg_override_en--;
        if (s->rep_override_en)
            s->rep_override_en--;

        /* 5. Decode instruction at CS:IP */
        decode_instruction(s, t, &d);

        /* 6. Execute */
        execute_instruction(s, p, t, &d);
```

Renumber the step comments (5 becomes "Decode", 6 becomes "Execute", etc.) to keep the sequence readable. Remove the old decrement block between decode and execute.

### Phase 4 — Update the LEA comment

In `run.c:233-239`, the comment says:

> Decremented to 0 before next instruction executes, so no CPU-visible effect.

That's now correct in effect (because of Phase 3), but the *wording* is still confusing. Rewrite to something like:

```c
/* LEA idiom: reference sets seg_override_en=1 as a side effect of
 * its "compute offset via segment-override machinery" trick. We
 * don't use that trick (we reconstruct the offset directly), but we
 * set the flag anyway to keep internal state aligned for the
 * harness comparator. The flag is decremented to 0 at the top of
 * the NEXT step's loop, before that instruction's decode, so the
 * override has no effect on the next instruction's effective-address
 * computation. Prior to EMU-30 the decrement ran after decode,
 * causing a leak into the next instruction. See docs/notes/8086tiny-quirks.md. */
```

### Phase 5 — Optional: extend pre-step snapshot to include seg_override

In `harness/harness.c`, find the divergence-report code that prints the pre-step block (look for "Pre-step state" fprintf). Extend the "Status" line to also print `seg_override`:

```c
fprintf(stderr, "Status           : int8_asap=%d seg_ovr_en=%d seg_ovr=%d rep_ovr_en=%d TF=%d\n",
        pre_ref->int8_asap,
        pre_ref->seg_override_en,
        pre_ref->seg_override,
        pre_ref->rep_override_en,
        pre_ref->trap_flag);
```

This is a small hygiene improvement, not required for EMU-30 to fix the bug. If it would take significant time (e.g., the StepSnapshot struct needs expansion), skip it — EMU-30's scope is the decrement reordering. Flag any skip in the commit message.

### Phase 6 — Unit tests

Existing tests should still pass unchanged. The behaviour change is subtle (ordering of internal state updates) and shouldn't affect any per-instruction unit test output.

Add ONE new integration-style test that exercises the LEA-then-memory-access sequence:

- Setup: pre-fill memory such that SS:[BP+6] contains one value (say 0x0516) and some_other_seg:[BP+6] contains a different value (say 0x8002).
- Execute: LEA at the start (just to set seg_override_en=1 via the side-effect path), then MOV AX, [BP+6].
- Assert: AX should be 0x0516 (read from SS, the default for BP-based addressing), not 0x8002.

The test is deliberately contrived — it's a regression guard for today's bug, not a natural test case. Put it in the most appropriate test file (likely `test/unit/test_run.c` or similar — grep for where LEA tests live).

### Phase 7 — Verify

```
cd packages/emu86
make test
```

All tests pass.

```
make emu86
./emu86 reference/bios test/images/freedos.img
```

Reaches FreeDOS banner.

```
touch src/emulator/run.c && make harness
./harness/harness reference/bios test/images/freedos.img
```

Expected outcome: harness advances past step 1,103,526. Report where it goes next. Possible outcomes:
- Another divergence at some step N > 1,103,526: report it as EMU-31.
- Clean run to step limit 200,000,000: huge milestone, report it explicitly.
- Harness completes FreeDOS boot cleanly and hits a natural idle/halt state: even bigger milestone.

### Phase 8 — Commit

Commit if:
- Phase 1 confirmed the baseline divergence (pre-fix).
- Phase 7 harness advances past step 1,103,526.
- Full make test green.
- `git status` shows only:
  - `src/emulator/run.c` modified (decrement moved, LEA comment updated)
  - `test/unit/test_run.c` modified (new regression test) — or wherever you added it
  - `harness/harness.c` modified if you did Phase 5
  - `tasks/emu30-task.md` created
  - No binaries, no reference/ changes, no Makefile changes

Commit message:

```
EMU-30: Decrement prefix override counters before decode

The main step loop in run.c was decrementing seg_override_en and
rep_override_en AFTER decode, but the reference 8086tiny decrements
them BEFORE decode. Consequence: in cases where seg_override_en
enters a step with value 1 (most notably the LEA side-effect set
by EMU-22's handler), our decode saw it as still active and
applied an override to that step's effective-address computation.
The reference's decode saw it already decremented to 0 and used
the default segment.

The harness caught the divergence at step 1,103,526 on a
MOV AX, [BP+0x06] immediately after an LEA. Our side read from
the stale override segment; reference read from SS (the default
for BP-based addressing).

The fix is a two-block reorder: move the seg_override_en and
rep_override_en decrements from their current position (after
decode, before execute) to just before decode. LEA's
seg_override_en=1 side-effect is now decremented to 0 at the
top of the next step's loop, before that step's decode runs,
matching the reference.

Also updated the misleading comment on the LEA handler that
described the decrement as "before next instruction executes" —
the actual ordering is "before next instruction DECODES", and
that matters.

Scope:
- src/emulator/run.c: two decrement blocks moved; LEA comment
  corrected.
- test/unit/test_run.c: regression test for LEA-then-memory-access.
- [Optional: harness/harness.c: pre-step snapshot format extended
  to include seg_override.]

Verification:
- Full make test green
- Standalone emu86 reaches FreeDOS banner
- Harness advances past step 1,103,526 (next divergence at step
  {N}, or clean to step limit).

Follow-up:
- EMU-31: whatever the harness finds after 1,103,526.
```

Task log entry:

```
## EMU-30
Date: {today}
Status: PASS
Test results: unchanged + 1 regression test
Harness: advances past step 1,103,526; next at step {N}.
Notes: Moved seg_override_en/rep_override_en decrements from
after-decode to before-decode. Fixes the "LEA side-effect
seg_override_en=1 leaks into next instruction's decode" bug.
The comment on the LEA handler incorrectly claimed "no CPU-
visible effect" from the flag-set; that was true only if the
decrement happened before decode, which it didn't.
```

Then:

```
mv tasks/emu30-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 9 — Report (on failure)

Triage to `tasks/triage/emu30-triage-report.md` if:
- Phase 1 shows a different divergence (baseline drifted unexpectedly).
- Phase 7 harness still halts at step 1,103,526 (the decrement move didn't fix it; something else is going on with segment overrides).
- Phase 7 harness gets further but regresses elsewhere (the reordering broke something).
- Full test suite fails in unexpected places.

## Out of scope — do not touch

- LEA's seg_override_en=1 setting itself — that's EMU-22's alignment and still needed.
- exec_segment_override and exec_rep_prefix — still set to 2.
- REP re-increment at run.c:367-371.
- Any Intel-defined-flag behaviour for any instruction.
- Any harness scaffolding beyond the optional Phase 5 snapshot extension.
- Dormant concerns.

## Final note

The bug has been present since EMU-22 landed (two days ago). It was masked for ~1.1M steps because the specific sequence "LEA followed by memory-accessing instruction" apparently didn't occur before step 1,103,526 in a way that produced observable divergence. That's interesting on its own — it suggests LEA followed by non-memory instructions was common enough to exercise the LEA-sets-flag path, but LEA followed by memory access with a default-SS was rare enough to hide the override-leak until deep into FreeDOS runtime.

Failure modes to watch for:

1. **Breaking the genuine-prefix case.** The reordering needs to preserve correct behaviour for real segment-override prefixes (`ES: MOV ...`). The Phase 3 analysis of Case A shows this works, but verify by ensuring any existing test involving segment overrides still passes. grep for `seg_override` in test files.

2. **The REP re-increment.** Line 367-370 re-increments seg_override_en inside the REP prefix handler. With decrement now at top-of-loop, this re-increment's role is to cancel the top-of-loop decrement so a segment override preceding a REP-string sequence stays alive through the string operation. This should still work with the new ordering — but verify by thinking through a REP MOVSB with ES: prefix scenario.

3. **LEA setting value 1 vs 2.** Current LEA sets seg_override_en=1. With decrement-before-decode at top of next step, that 1 becomes 0 before next decode. Good. If we'd accidentally set it to 2, it would become 1, decode would apply override, and we'd be back to the current bug (just shifted by one instruction). Keep the value at 1.

The pattern "our emulator had a subtly-wrong ordering of internal bookkeeping that manifested only in specific instruction sequences" is worth remembering. Previous divergences have been arithmetic/flag bugs in specific instructions. This one is structural — an invariant about when state transitions happen. Different shape of bug, possibly another instance lurking elsewhere (though nothing obvious).
