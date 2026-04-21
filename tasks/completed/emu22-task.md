# EMU-22: Complete LEA alignment — seg_override_en side effect + comparator REG_ZERO equivalence

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are *reference material*, not your assignment. Your assignment is this document.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Required reading before you touch code

**`tasks/triage/emu21-triage-report.md`** — written by the previous agent session. Essential. It documents:
- The EMU-21 partial fix (Approach 1 in `run.c` case 10) that correctly aligns `seg_override_en` between our emulator and the reference.
- The second-order divergence it unmasked: with `seg_override_en = 1` on both sides, the comparator now checks `seg_override` values, which differ (ref=12=REG_ZERO, ours=1=stale SREG_CS).
- A secondary finding: `exec_lea` in `opcodes/transfer.h` is dead code on the runtime path. (Deferred — EMU-23 territory.)
- Three alignment options for the remaining mismatch. The previous agent's weak preference was Option 2 (comparator equivalence class for REG_ZERO). This brief commits to that option.

Read the report fully before proceeding. The specific evidence (line numbers, before/after comparator output, the REG_ZERO=12 / 0xFF conversion detail) will save you re-investigating.

Also read:

- **`docs/notes/8086tiny-quirks.md`**, especially the existing "AF flag after shift instructions" entry — structure the new quirks entry similarly.
- **`packages/emu86/harness/harness.c`**, the comparator function (`compare_states`) and the Prefix-state comparison block (~line 401–404). Focus on `ref_to_ours_sreg()` and where `seg_override` is read.
- **`packages/emu86/src/emulator/run.c`**, case 10 (opcode 0x8D, LEA) — the runtime LEA implementation, lines ~220–232.

### What this task does

Close the step-65,770 divergence completely, in two coordinated changes:

1. **Emulator side:** Add `s->seg_override_en = 1` at the end of LEA's runtime handler (`run.c` case 10). This aligns our emulator's internal state with the reference's transient override flag.

2. **Harness side:** Teach the comparator that when the reference's `seg_override` holds the REG_ZERO sentinel (value 12), the corresponding field on our side is "don't care" — the sentinel is an implementation-specific marker for the LEA idiom and has no guest-observable meaning. Comparing it produces false positives.

Both changes are necessary. The emulator change alone (as EMU-21 demonstrated) unmasks the `seg_override` value mismatch. The harness change alone doesn't address the `seg_override_en` mismatch. Together they close the step-65,770 divergence.

### Scope clarifications

- **Do not** add a zero-segment pseudo-register to `Emu86State` (Option 1 / "Approach 2" in EMU-21). The comparator-side fix achieves the same lockstep property without carrying permanent state for a single-instruction idiom.
- **Do not** fix the `exec_lea` dead code issue in `transfer.h`. That's EMU-23. Resist the urge.
- **Do not** rework decrement ordering. The EMU-21 triage noted that our decrement timing differs subtly from the reference's, but it doesn't produce incorrect behaviour in any observed trace. Leave it.

## Your task

### Phase 1 — Verify the starting state

Confirm the current tree reproduces the step-65,770 divergence with both `seg_override_en` AND `seg_override` differing:

```
cd packages/emu86
make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected (matching the triage report):
- Divergence at step 65,770.
- CS:IP `1FE0:7C65` after the step.
- Category 0x00000010 (DIV_PREFIX).
- `seg_override_en`: ref=1, ours=0.
- `seg_override`: ref=12 (→255), ours=1.

If the output doesn't match, stop and report — something changed since the triage report was written.

### Phase 2 — Apply the emulator-side fix

In `packages/emu86/src/emulator/run.c`, case 10, at the end of the LEA branch (after the offset is written to the destination register), add one line:

```c
s->seg_override_en = 1;
```

Add a comment referencing the quirks doc:

```c
/* LEA idiom: reference sets seg_override_en=1 as a side effect of
 * its "compute offset via segment-override machinery" trick. We
 * don't use that trick (we reconstruct the offset directly), but we
 * set the flag anyway to keep internal state aligned for harness
 * lockstep. Decremented to 0 before next instruction executes, so
 * no CPU-visible effect. See docs/notes/8086tiny-quirks.md. */
s->seg_override_en = 1;
```

Do not modify `exec_lea` in `opcodes/transfer.h` — it's dead code on the runtime path (EMU-23 will resolve).

### Phase 3 — Apply the harness-side fix

In `packages/emu86/harness/harness.c`, locate the Prefix-state comparison block in `compare_states` (~line 400-405):

```c
uint8_t ref_so = ref_to_ours_sreg(seg_override);
if (our_state->seg_override_en != seg_override_en ||
    (seg_override_en && our_state->seg_override != ref_so) ||
    our_state->rep_override_en != rep_override_en ||
    (rep_override_en && our_state->rep_mode != rep_mode))
    flags |= DIV_PREFIX;
```

The current logic: when `seg_override_en` is set on both sides, compare `seg_override` values via `ref_to_ours_sreg()` (which returns 0xFF for REG_ZERO=12). Our `seg_override` can never equal 0xFF, so any occurrence of ref using REG_ZERO causes DIV_PREFIX to fire.

The fix: treat `ref seg_override == REG_ZERO (12)` as an equivalence-class match against any value of our `seg_override`. Concretely:

```c
/* REG_ZERO (12) is the reference's sentinel for LEA's offset-
 * computation trick. It has no guest-observable meaning — the
 * corresponding seg_override_en is transient and decrements to 0
 * before any instruction consumes it. Our emulator has no
 * equivalent sentinel (and doesn't need one, since we compute LEA
 * offsets directly). Treat REG_ZERO as "don't compare" for the
 * seg_override field. See docs/notes/8086tiny-quirks.md. */
#define REG_ZERO 12
uint8_t ref_so = ref_to_ours_sreg(seg_override);
int seg_override_differs =
    seg_override_en
    && seg_override != REG_ZERO
    && our_state->seg_override != ref_so;

if (our_state->seg_override_en != seg_override_en ||
    seg_override_differs ||
    our_state->rep_override_en != rep_override_en ||
    (rep_override_en && our_state->rep_mode != rep_mode))
    flags |= DIV_PREFIX;
```

(Or an equivalent restructuring that achieves the same semantics — the exact code shape is your call, as long as the behaviour is: when `ref`'s `seg_override == REG_ZERO`, skip the seg_override value comparison.)

Also check: is `REG_ZERO` already defined somewhere in the harness file? The triage report mentioned it at `8086tiny.c` side. If there's an existing definition from the reference side, use that instead of re-defining it. Don't duplicate the constant.

### Phase 4 — Update the Prefix-state *reporting* to match

In the `report_divergence` function, the Prefix-state reporting block (around line 521-532 per the triage report) prints all four prefix fields whenever `DIV_PREFIX` fires. With the Phase 3 change, the `seg_override` field may *match logically* (equivalence class) but still appear textually different in the report.

Make sure the divergence report remains honest. Two options:

**Option A:** Change the report to print "equivalent (REG_ZERO)" when ref uses REG_ZERO and ours has any value. Requires a small change in the reporter.

**Option B:** Leave the report as-is (prints the raw values regardless of equivalence class). Accept that when REG_ZERO is in play but `seg_override` is the *only* thing diverging, DIV_PREFIX won't fire — so the misleading report-line won't appear. It only matters if DIV_PREFIX fires for *another* reason (e.g., `rep_override_en` differs too), in which case the seg_override line shows confusingly.

Option B is simpler and the case where it matters is edge-case. Prefer Option B unless the implementation naturally lends itself to A.

### Phase 5 — Regression tests

Add unit tests to `packages/emu86/test/unit/` — either extending an existing test file or creating a new one (agent's judgement). Required tests:

**Emulator-side (testing the Approach 1 fix):**

- **LEA sets seg_override_en = 1.** Starting with `seg_override_en = 0`, execute a LEA instruction (any form) via the runtime path (i.e., through `emu86_run` or the equivalent step function, *not* by calling `exec_lea` directly — that function is dead code). Verify `seg_override_en = 1` afterwards.

- **LEA result is unchanged.** Regression guard. Construct a LEA with known operands (e.g., `LEA BX, [DI+0x10]` with DI set to known value). Execute. Verify the destination register holds the correct offset value. This confirms the side-effect addition didn't break LEA's semantics.

- **LEA doesn't modify flags.** Regression guard. Set flags to a known pattern, execute LEA, verify flags are unchanged.

**Harness-side (testing the comparator equivalence):**

- This is harder to test in isolation because the comparator is called from within the harness's step-end hook, not easily reachable from unit tests. Two options:
  - **Option A:** Factor out the comparator logic into a pure function that takes both states as parameters, and unit-test that function. Cleaner, more future-proof, but structural.
  - **Option B:** Don't unit-test the comparator directly; rely on the harness's own self-test mode (the `inject_divergence` hook at `harness.c:644`) to exercise the comparator. Add a test harness run that deliberately sets our `seg_override` to a specific non-REG_ZERO value while ref uses REG_ZERO, and verifies no DIV_PREFIX fires.
  - **Option C:** Skip direct comparator tests; rely on the Phase 6 harness run to validate the change end-to-end.
  
  Your call. If unsure, default to Option C — the Phase 6 harness run is a strong integration test and this isn't deeply logic-heavy code.

**Revert-and-re-test:** For the emulator-side tests, confirm the "LEA sets seg_override_en = 1" test fails against the pre-fix code. Revert the fix, run tests, confirm it fails; re-apply, confirm it passes.

Run the full test suite. No existing test should regress.

### Phase 6 — Verify via harness

Re-run the harness with both fixes:

```
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

**Makefile quirk reminder:** `make harness` may not pick up header changes. If Phase 6 results don't match the fix, force a rebuild:

```
touch src/emulator/run.c harness/harness.c && make harness
```

Three outcomes:

**(a) Step-65,770 divergence is gone; harness runs further.** Expected. Record the new divergence in the commit's Follow-up section.

**(b) Step-65,770 divergence is gone; harness runs to the 200M-step limit.** Excellent — record the step count.

**(c) Divergence still at step 65,770.** Either fix is wrong or there's a third issue at the same instruction that EMU-21 didn't identify. Stop and report.

### Phase 7 — Standalone check

Confirm standalone `./emu86` still reaches the FreeDOS kernel banner:

```
./emu86 reference/bios test/images/freedos.img
```

If behaviour regresses, stop and report.

### Phase 8 — Update the quirks documentation

Append a new entry to `docs/notes/8086tiny-quirks.md`. Template:

```markdown
## LEA and the transient seg_override idiom (Intel-behaviourally-equivalent)

**Location:** `reference/8086tiny.c` LEA handling (case 10, `!i_d`
branch), our `packages/emu86/src/emulator/run.c` case 10, and our
`packages/emu86/harness/harness.c` comparator.

**Behaviour:** The reference implements LEA's offset computation by
reusing the segment-override machinery. It sets:
- `seg_override_en = 1` (a counter that decrements at the top of the
  next main-loop iteration, before any instruction consumes it)
- `seg_override = REG_ZERO` (value 12, a pseudo-register at
  `regs16[REG_ZERO]` that is initialised to 0 and never written to)

After these are set, `SEGREG(REG_ZERO, ...)` evaluates to
`0*16 + offset = offset`, which is what LEA needs in rm_addr.

At the step boundary after LEA executes, the reference has both fields
set. The next main-loop iteration decrements seg_override_en to 0,
and seg_override is never consulted. CPU-observable behaviour is
identical to a conventional LEA implementation.

**Our implementation** (`run.c` case 10) reconstructs the offset
directly from `rm_addr - segment*16`, without using the segment-
override machinery. To align the internal state visible at step
boundaries, we set `seg_override_en = 1` at the end of LEA as a
side effect (matching the reference's flag setting). We do *not*
set `seg_override` to any sentinel value, because our state
vocabulary has no REG_ZERO equivalent — our `seg_override` is an
SREG enum in [0, 3].

**Harness compensation:** The comparator in `harness/harness.c`
treats `ref seg_override == REG_ZERO (12)` as an equivalence-class
match: when the reference uses this sentinel, the comparator skips
the seg_override value comparison. This is a principled exception
because REG_ZERO is an implementation-specific marker with no
guest-observable meaning.

**Category:** Intel-behaviourally-equivalent divergence (like the
AF-on-shift alignment). Both implementations produce identical CPU
behaviour but differ in internal state visible at step boundaries.
In this case, unlike AF-on-shift, the alignment required a two-part
fix: emulator side (set the flag) and harness side (don't compare
the sentinel value).

**Why not add a zero-segment register to our state?** That was the
alternative approach (extend `Emu86State.sregs` to 5 entries, add
`SREG_ZERO = 4`, etc.). Rejected because it adds permanent state
for a single-instruction idiom. The harness-side equivalence class
achieves the same lockstep property without carrying that weight.
Future LEA-adjacent alignments that need the same equivalence
benefit from this foundation; future alignments that don't can
ignore it.

**Identified during:** EMU-21 (partial, emulator side) and EMU-22
(completed, harness side). EMU-21 triage report at
`tasks/triage/emu21-triage-report.md` has the full investigation.

**Related:** `exec_lea` in `opcodes/transfer.h` is dead code on the
runtime path — the runtime LEA is inlined in `run.c` case 10. This
was discovered during EMU-21. Resolving the DRY violation is
deferred to EMU-23.
```

Adjust wording if it reads awkwardly.

### Phase 9 — Commit (on success) or report (on failure)

**Commit** if: Phase 1 reproduced both field divergences, Phases 2-3 applied both fixes cleanly, Phase 5 tests are correct (fail-before, pass-after for the emulator-side tests), Phase 6 produced outcome (a) or (b), Phase 7 confirmed standalone, and Phase 8 updated the quirks doc.

Commit message:

```
EMU-22: Complete LEA alignment — seg_override_en side effect + harness REG_ZERO equivalence

Context: EMU-21 identified that LEA has two independent divergences
         at step 65,770 — seg_override_en (closed by a one-line
         emulator fix) and seg_override value (ref=12=REG_ZERO vs
         ours=1=stale). EMU-21 was triaged as outcome (d) because
         closing only the first unmasked the second. This task
         applies both halves of the fix.

Emulator change: run.c case 10 (LEA) now sets s->seg_override_en=1
                 as a side effect, matching the reference's
                 LEA-via-segment-override trick. Decremented to 0
                 before the next instruction's decode, so no
                 guest-visible effect.

Harness change: compare_states now treats ref seg_override==REG_ZERO
                as an equivalence-class match. REG_ZERO is the
                reference's sentinel for this LEA idiom and has no
                guest-observable meaning — our emulator has no such
                sentinel and doesn't need one (we compute LEA offsets
                directly). Comparing it produced false positives.

Tests: N new tests in test_*.c:
       - LEA sets seg_override_en via runtime path (fail-before, pass-after)
       - LEA result value unchanged (regression guard)
       - LEA flag-neutrality (regression guard)
       [harness-side tests: whatever Phase 5 chose]

Harness result: [outcome (a) or (b); step count reached and next
                 divergence if any]

Standalone: unchanged — FreeDOS kernel banner reached.

Docs: Added "LEA and the transient seg_override idiom" entry to
      docs/notes/8086tiny-quirks.md. Subsumes the partial
      documentation that EMU-21 would have written.

Follow-up:
- EMU-23: resolve exec_lea dead code in opcodes/transfer.h.
  Either delete it and its unit tests, or wire it into run.c
  case 10 and fix its latent hardcoded-DS bug.
- [next divergence from Phase 6]
```

Task log entry:

```
## EMU-22
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — {N} new tests
Harness result: {step count reached, new divergence if any}
Notes: Completed the LEA alignment started in EMU-21. Emulator side:
seg_override_en=1 side effect in run.c case 10. Harness side:
REG_ZERO equivalence class in compare_states. Intel-behaviourally-
equivalent divergence, two-part fix required. Quirks doc updated.
Standalone ./emu86 unchanged. Follow-up EMU-23 identified for
exec_lea dead code.
```

Then:

```bash
mv tasks/emu21-task.md tasks/completed/    # finally close EMU-21 too
mv tasks/emu22-task.md tasks/completed/
git add -A
git commit
```

(Yes — EMU-21's task file never moved to `completed/` because EMU-21 was triaged rather than committed. The EMU-21 work's outcome is captured in the triage report, which stays in `tasks/triage/`. Moving the task file now reflects that EMU-21 has been effectively closed by this task's successful completion of its aims.)

Do not push. User reviews.

**Report** to `tasks/triage/emu22-triage-report.md` if: Phase 1 doesn't reproduce, Phase 2 or 3 needs structural changes beyond the prescribed lines, Phase 5 tests can't be made to fail-before-and-pass-after, Phase 6 produces outcome (c), or Phase 7 shows standalone regression.

## Out of scope — do not touch

- **`exec_lea` dead code in `opcodes/transfer.h`.** That's EMU-23. The triage report's detailed finding makes it tempting to also fix that as a one-liner; resist.
- **Any new divergence the harness finds after the fix.** Next task.
- **Adding a zero-segment pseudo-register to state.** Alternative approach explicitly rejected.
- **Reworking decrement ordering.** The triage noted our ordering differs subtly from the reference's but doesn't cause wrong behaviour. Leave it.
- **Harness enhancements** beyond the REG_ZERO equivalence (e.g., report-all-divergences-per-category). Follow-up work.
- Previous out-of-scope items: `editor-api-proposal.md`, 0xEA length bug, 0xC0/0xC1 rotate-form reference bug, silent-exit-on-0:0, register-memory aliasing, timer design questions, FreeDOS divide-by-zero.

## Housekeeping

- Scratch files in `/tmp/emu86-harness/` per EMU-17. Leave them.
- `packages/emu86/emu86-dbg` may still be present. Leave it.
- The harness `opcode bytes at CS:IP` instrumentation (added during EMU-21 preparation) must be retained — it's genuinely useful for future divergence investigations. Don't remove it. If it's missing, re-add it per the EMU-21 brief's Phase 8.
- No new binaries in the commit. Verify `git status` before `git add`.
- **Makefile quirk:** `make harness` doesn't track opcode-header or harness-source dependencies reliably. Force rebuilds with `touch src/emulator/run.c harness/harness.c && make harness` if Phase 6 results seem stale.

## Final note

This is the first task in the project that explicitly coordinates an emulator-side change with a harness-side change. Until now, the harness has been a passive observer — the thing that detects bugs in our emulator. This task uses the harness as an active participant in alignment: we change how the harness compares state, not just what our emulator produces.

This is a defensible move, but it sets a precedent worth noting. Future "Intel-behaviourally-equivalent" divergences may invite similar harness changes. That's fine in the near term but has a slippery-slope property: if every awkward alignment gets a bespoke harness exception, the harness accumulates reference-specific knowledge and becomes harder to reason about. Watch for it.

For this task specifically: the REG_ZERO equivalence is a *principled* exception — REG_ZERO is a documented sentinel with no guest-observable meaning. It's not "the harness is wrong about this one case," it's "the harness should reflect the reference's semantics, and this sentinel has no semantics." That framing justifies the change.

If during implementation the equivalence starts feeling like an ad-hoc patch rather than a principled exception, stop and report. Better to re-scope than to land a change that will age badly.

Pay particular attention to the Phase 5 tests. Testing "a side effect that isn't guest-observable" is the kind of thing that invites toy tests. Make sure the tests exercise the code path the harness actually depends on — runtime LEA, runtime decrement, runtime state visibility at step boundaries.
