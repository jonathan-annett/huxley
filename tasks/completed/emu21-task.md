# EMU-21: Align LEA seg_override_en behaviour with reference

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** You will find other task files in `tasks/completed/` and triage reports in `tasks/triage/`. Those are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Where we are

The differential test harness (EMU-16/17/19/20) runs our emulator in lockstep with the reference `8086tiny` and halts on any divergence. EMU-20 aligned SHL/SHR/SAR AF behaviour with the reference (Intel-undefined AF after shifts). The harness now reaches step 65,770 before reporting a new divergence.

### The divergence, correctly attributed

The divergence report at step 65,770 shows `CS:IP = 1FE0:7C65`, opcode bytes `FB 80 7E 24 FF 75 03 88`, with the Prefix category flagged and `seg_override_en` differing (ref=1, ours=0).

**The reported CS:IP is after the step, not at it** — same misdirection pattern as EMU-20. Step-limit 65,769 returns cleanly at CS:IP `1FE0:7C62` with opcode bytes `8D 66 A0` (the bytes we care about were obtained via a small harness instrumentation that now prints opcode bytes at step-limit exit — see "Keep the harness instrumentation" below).

The divergent instruction is therefore `8D 66 A0` at `1FE0:7C62`, which decodes as:

- `8D` — LEA r16, m (Load Effective Address)
- `66` — modrm byte: mod=01, reg=100 (SP), rm=110 ([BP]+disp8)
- `A0` — 8-bit displacement (signed -0x60)

So the instruction is **`LEA SP, [BP - 0x60]`**. Three bytes total. IP advances from `1FE0:7C62` to `1FE0:7C65`. Standard C function-epilogue stack-frame teardown.

### The root cause — theological in nature

Neither emulator is "wrong" in any observable-behaviour sense. The divergence is an internal-state difference caused by two defensible implementations of LEA producing identical CPU behaviour but different intermediate state that the harness sees at step boundaries.

**The reference's LEA trick** (`reference/8086tiny.c` around line 447-451):

```c
else if (!i_d) // LEA
    seg_override_en = 1,
    seg_override = REG_ZERO,
    DECODE_RM_REG,
    R_M_OP(mem[op_from_addr], =, rm_addr);
```

Cable sets `seg_override_en = 1` and forces `seg_override = REG_ZERO` (a pseudo-register holding 0). This reuses the existing segment-override machinery to compute `SEGREG(REG_ZERO, ...)` effectively as `0*16 + offset = offset`. The result: rm_addr ends up being the raw offset, which is what LEA needs.

In the reference, `seg_override_en` is a **counter** that decrements at the top of every main-loop iteration (line 322-323). So after LEA executes, `seg_override_en = 1` at the end of that step. On the *next* step, the top-of-loop decrement brings it to 0, never actually affecting the next instruction's segment resolution (no override applies to the next instruction because it's decremented before being consulted).

**Our emulator's LEA** (`packages/emu86/src/emulator/opcodes/transfer.h`, `exec_lea`):

```c
if (s->seg_override_en)
    segment = s->sregs[s->seg_override];
else {
    segment = s->sregs[SREG_DS];  /* placeholder */
}
write_reg16(s, d->reg, (uint16_t)(d->rm_addr - ((uint32_t)segment << 4)));
```

We compute the offset directly by reconstructing it (subtract the segment base from the linear address) rather than using Cable's set-seg-override trick. This works for our semantic correctness, but it means our LEA has no side effect on `seg_override_en`.

At the end of step 65,770: reference has `seg_override_en = 1`, ours has `seg_override_en = 0`. The next step's top-of-loop decrement would bring ref's to 0, and nothing would consume the override, so the visible CPU behaviour (subsequent instruction results) is identical. But at this step's boundary, the harness comparator sees the difference and halts.

### The pattern forming

This is the second "Intel-behaviourally-equivalent divergence" the harness has found. The first was EMU-20's AF-on-shift — both implementations produced different AF values after SHL/SHR/SAR, both were defensible per Intel, and we aligned by matching the reference.

This one is similar in character: different internal state, identical observable behaviour. The fix pattern is the same: match the reference's approach so the harness stays useful.

It's worth flagging that this class of divergence is going to keep showing up until our implementation matches the reference's approach to internal state transitions closely enough that step-boundary comparisons don't catch implementation-style differences. At some point we may want to improve the harness to understand "these fields are equivalent at step boundary," but for now the narrow-alignment approach keeps the harness simple and our emulator close to the reference.

## Your task

### Phase 1 — Verify the state

Confirm the current tree reproduces the step-65,770 divergence:

```
cd packages/emu86
make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected: divergence at step 65,770, category Prefix, `seg_override_en` differing (ref=1, ours=0), CS:IP after the divergent step is `1FE0:7C65`.

Also confirm the pre-step state:

```
HARNESS_STEP_LIMIT=65769 ./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -5
```

Expected: "reached step limit 65769" at CS:IP `1FE0:7C62`, with the harness's opcode-bytes instrumentation printing `8D 66 A0 FB 80 7E 24 FF` (the first three bytes are the divergent LEA).

If either doesn't reproduce as described, stop and report.

### Phase 2 — Verify this is the only divergence

The divergence report shows category Prefix and names `seg_override_en`, but the harness's comparator breaks early on the first divergence of each category. It's possible other state also differs at step 65,770 and the report just didn't show it because of the early-break.

Before applying the fix, verify the LEA-side-effect alignment is sufficient to close this divergence. Approaches, in rough preference order:

**Option A (quickest):** Apply the fix, run the harness, observe whether the divergence at step 65,770 is gone. If it's gone (outcome a or b in Phase 5), the fix was sufficient. If a new divergence appears at step 65,770 in a *different* category, the fix was partial and more alignment work is needed.

**Option B (more thorough):** Modify the harness's comparator temporarily to report *all* diverging categories and all diverging fields within each, not just the first. Run at step 65,770 with the original code. Confirm the only divergence is `seg_override_en`. Revert the comparator change. Then apply the fix.

Your choice. Option A is pragmatic; Option B is rigorous. If Option A produces a surprising result (new divergence at the same step), switch to Option B.

### Phase 3 — Apply the fix

In `packages/emu86/src/emulator/opcodes/transfer.h`, modify `exec_lea` to set `seg_override_en = 1` and `seg_override` to the zero-segment-equivalent before (or instead of) reconstructing the offset directly.

The specific change depends on what "zero-segment-equivalent" means in our state representation. The reference uses `REG_ZERO`, which is a pseudo-register at a specific offset in its register file that holds 0. Our emulator has `Emu86State.sregs[4]` (ES, CS, SS, DS). There is no zero-segment register in our state struct.

Three approaches:

**Approach 1 (minimal state change):** Set `seg_override_en = 1` and leave `seg_override` pointing at whatever it already was (or an arbitrary valid value). Rely on the existing decrement mechanism to clear `seg_override_en` before the next instruction executes. The *value* of `seg_override` doesn't matter because nothing consults it before decrement. Keep our existing offset-reconstruction logic so the result is still correct without actually using seg_override for the computation. Simplest fix — just add the `seg_override_en = 1` side effect. Downside: slight semantic confusion (why is seg_override_en set if we're not using it?). Mitigated by a clear comment.

**Approach 2 (match reference more closely):** Add a zero-segment register equivalent to our state. Set `seg_override_en = 1` and point `seg_override` at it. Compute the offset via the segment-override path, matching the reference. Larger change — requires adding state, changing init, potentially updating snapshot serialisation.

**Approach 3 (reference as oracle, embrace the trick):** Acknowledge that LEA's "hack" of transient seg_override is an 8086tiny idiom we can inherit. Add a comment documenting why the side effect exists, implement with Approach 1, and move on.

My recommendation is Approach 3 (implemented as Approach 1). The fix is one or two lines in `exec_lea`. Document the rationale with a comment that references `docs/notes/8086tiny-quirks.md`.

**Constraint:** the change must not alter the *result* of LEA (the value written to the destination register). The harness will catch this as a register divergence if it happens. Before and after the fix, LEA should produce the same value for any given inputs.

### Phase 4 — Regression tests

Add unit tests to `packages/emu86/test/unit/test_transfer.c` (or wherever LEA tests live, or a new file):

- **LEA sets seg_override_en = 1 as side effect.** Starting with `seg_override_en = 0`, execute LEA, verify `seg_override_en` is 1 afterwards.
- **LEA's destination register value is correct.** This is a regression guard for Phase 3's constraint — the fix must not change the value LEA computes. Run LEA with known inputs and verify the output register value matches the expected offset.
- **LEA does not modify flags.** Regression guard — LEA is flag-neutral; ensure the fix doesn't accidentally change that.

Revert-and-re-test discipline: confirm the seg_override_en side-effect test fails against the pre-fix code and passes against the post-fix code. The destination-value and flag-neutral tests should pass both before and after.

Run the full test suite. No existing test should regress.

### Phase 5 — Verify via harness

Re-run the harness:

```
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Three outcomes:

**(a) Step-65,770 divergence is gone; harness runs further before finding a new divergence.** Expected outcome. Record the new divergence in the commit's Follow-up section. Do not fix it.

**(b) Step-65,770 divergence is gone; harness runs to the 200M-step limit cleanly.** Excellent — record the step count.

**(c) Divergence still at step 65,770 (or earlier).** The fix was insufficient or wrong. Stop and report.

**(d) Divergence at step 65,770 but now in a different category/field** (e.g., GP register, or a different seg_override field). The LEA side-effect fix was correct but there's another issue at the same instruction. Document what you found and stop — don't patch it here.

### Phase 6 — Standalone check

Confirm standalone `./emu86` still reaches the FreeDOS kernel banner:

```
./emu86 reference/bios test/images/freedos.img
```

If behaviour regresses, stop and report.

### Phase 7 — Update the quirks documentation

Append a new entry to `docs/notes/8086tiny-quirks.md`. Template:

```markdown
## LEA and transient seg_override_en (implementation idiom)

**Location:** `reference/8086tiny.c` LEA handling (case 10, `!i_d` branch)
and our `packages/emu86/src/emulator/opcodes/transfer.h` exec_lea.

**Behaviour:** The reference implements LEA's effective-address
computation by reusing the segment-override machinery. It sets
`seg_override_en = 1` and `seg_override = REG_ZERO` (a pseudo-register
holding 0), so that the subsequent address-decode macros compute
`0*16 + offset = offset` and land that in rm_addr. The
`seg_override_en = 1` persists past LEA's execution and is
decremented to 0 at the top of the next main-loop iteration, before
any subsequent instruction could consult it. This is invisible to
guest software — CPU observable behaviour is identical to a
conventional LEA implementation.

**Our previous implementation** reconstructed the offset directly by
subtracting the segment base from the computed linear address. No
`seg_override_en` side effect. Semantically equivalent; visibly
different at step boundaries.

**What we do:** EMU-21 aligned `exec_lea` to set `seg_override_en = 1`
matching the reference. The existing decrement mechanism handles
clearing it before the next instruction. Our LEA still computes the
offset directly (we don't route through the seg_override machinery
for the computation itself) — we only match the side effect.

**Category:** Intel-behaviourally-equivalent divergence. Like the
AF-on-shift alignment (see above), this is a case where both
implementations produce identical observable CPU behaviour but differ
in internal state visible to the harness at step boundaries. Matching
the reference keeps the harness useful.

**Identified during:** EMU-21 investigation (following a harness
divergence at step 65,770 during FreeDOS kernel execution).
```

Adjust wording to fit context.

### Phase 8 — Keep the harness instrumentation

During preparation for EMU-21, an earlier session added three lines to `harness/harness.c`'s step-limit-exit block to print opcode bytes at CS:IP. These lines are useful for future divergence investigations (they make every step-limit exit self-documenting about what was about to execute). Do not remove them.

If the current tree already has those lines (check for "opcode bytes at CS:IP" in harness.c), leave them alone. If they're missing somehow, add them back — this is part of EMU-21 even if it wasn't part of the pre-task work.

The three lines go just before the `fflush(stderr)` in the step-limit-exit block:

```c
uint32_t lin = ((uint32_t)regs16[REF_REG_CS] << 4) + reg_ip;
fprintf(stderr, "harness: opcode bytes at CS:IP: ");
for (int i = 0; i < 8; i++) fprintf(stderr, "%02X ", mem[lin + i]);
fprintf(stderr, "\n");
```

### Phase 9 — Commit

**Commit** if: Phase 1 reproduced the bug, Phase 2 confirmed the fix should close this specific divergence, Phase 3 applied a minimal fix (one or two lines added to exec_lea; no structural changes), Phase 4's regression tests fail-before-and-pass-after, Phase 5 produced outcome (a) or (b), Phase 6 confirmed standalone behaviour, Phase 7 updated the quirks doc, and Phase 8 confirmed (or added) the harness instrumentation.

Commit message:

```
EMU-21: Align LEA seg_override_en side effect with reference

Bug: LEA in the reference sets seg_override_en = 1 as a side effect
     (implementation trick for offset computation via the segment-
     override machinery). Our LEA doesn't. At step boundaries, the
     harness sees seg_override_en differ — ref=1 ours=0 — even though
     CPU behaviour is identical (seg_override_en is decremented to 0
     at the top of the next iteration before any instruction uses it).

Fix: Added `s->seg_override_en = 1` to exec_lea in transfer.h.
     Semantically a no-op (nothing consumes it before decrement);
     aligns our internal state with the reference's for harness
     lockstep.

Tests: N new tests in test_transfer.c:
       - exec_lea sets seg_override_en (fail-before, pass-after)
       - exec_lea result value unchanged (regression guard)
       - exec_lea doesn't modify flags (regression guard)

Harness result: [outcome (a) or (b) — step count reached, next
                 divergence if any]

Standalone: unchanged — FreeDOS kernel banner reached.

Docs: Added `LEA and transient seg_override_en` entry to
      docs/notes/8086tiny-quirks.md.

Harness instrumentation: opcode-bytes output in step-limit-exit
                         block retained (useful for future
                         divergence investigations).

Follow-up: [next divergence from Phase 5]
```

Task log entry:

```
## EMU-21
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests for LEA side effects
Harness result: {step count reached, new divergence if any}
Notes: Aligned LEA's transient seg_override_en side effect with the
reference. Intel-behaviourally-equivalent divergence; matching the
oracle for harness lockstep. Quirks doc updated. Standalone ./emu86
unchanged. Harness instrumentation (opcode bytes at step-limit exit)
retained.
```

Then:

```bash
mv tasks/emu21-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

**Report** to `tasks/triage/emu21-triage-report.md` if: Phase 1 doesn't reproduce, Phase 2 reveals multiple divergences in unexpected categories, Phase 3 needs more than one or two lines, Phase 4 tests can't be made to fail-before-and-pass-after, Phase 5 produces outcome (c), or Phase 6 shows standalone regression.

## Out of scope — do not touch

- **Any new divergence the harness finds after the fix.** That's the next task.
- **Refactoring LEA** beyond adding the side effect. Do not replace the offset-reconstruction logic with the reference's seg-override computation, even though doing so would match the reference more closely. The brief's Approach 1 (minimal side effect only, keep our computation logic) is what's wanted.
- **Adding a zero-segment register** to state (Approach 2). Out of scope.
- **General harness improvements** (better divergence reporting, handling behaviourally-equivalent state differences structurally, etc.). Follow-up work.
- **The FreeDOS divide-by-zero.** Still pending.
- Previous out-of-scope items: `editor-api-proposal.md`, latent 0xEA JMP/CALL length bug, 0xC0/0xC1 rotate-form reference bug, silent-exit-on-0:0, register-memory aliasing, timer design questions.

## Housekeeping

- Scratch files in `/tmp/emu86-harness/` per EMU-17. Leave them.
- `packages/emu86/emu86-dbg` may still be present from EMU-14. Leave it.
- No new binaries in the commit. Verify `git status` before `git add`.
- **Makefile quirk:** `make harness` doesn't always rebuild when opcode headers change. If Phase 5's result seems stale or doesn't match the fix, force a rebuild with `touch src/emulator/run.c && make harness`. This is a known latent issue, noted during EMU-20.

## Final note

The fourth narrow-fix task in a row (EMU-15, EMU-19, EMU-20, EMU-21). The structure is now well-trodden: verify reproduction, apply minimal fix, regression tests with revert-and-re-test, harness verification, docs update, commit.

This specific divergence is worth being calm about. It's not a bug in any meaningful sense — both LEA implementations produce identical guest-observable behaviour. The fix aligns an implementation detail to keep the harness simple. If the fix doesn't close the divergence, that's more informative than the fix "working." Be ready for Phase 5 outcome (d) where another divergence emerges at the same step — that would mean there's a second independent alignment issue, also at LEA, which would be more interesting than the simple one this brief assumes.

Read the harness output carefully. Do the revert-and-re-test. If anything surprises you, stop and report.
