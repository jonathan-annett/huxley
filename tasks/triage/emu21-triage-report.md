# EMU-21 Report
Date: 2026-04-21
Outcome: **PHASE-5-OUTCOME-(d)** — partial fix correct, second independent issue revealed at the same step

## Summary

The EMU-21 brief's Approach 1 fix (set `seg_override_en = 1` in LEA's
handler) is correct and closes the `seg_override_en` field divergence at
step 65,770. But the same step contains a *second*, independent prefix-state
divergence that the brief did not anticipate: the `seg_override` field
itself differs. Ref has `seg_override = 12` (REG_ZERO); ours has
`seg_override = 1` (SREG_CS, stale from an earlier segment-override prefix).
After applying the Approach 1 fix, the harness still halts at step 65,770
with `DIV_PREFIX` — just for a different field in the same category. This
matches Phase 5 outcome **(d)**, which the brief instructs me to document
and stop on ("don't patch it here").

The remaining `seg_override` mismatch cannot be closed by a change confined
to `exec_lea`. It requires either (a) Approach 2 (adding a zero-segment
pseudo-register to `Emu86State`, explicitly out of scope per the brief), or
(b) a comparator-side allowance in `harness/harness.c` that recognises
REG_ZERO as an equivalence class (currently the comparator compares raw
values when `seg_override_en` is set).

Along the way I discovered a secondary issue the brief was unaware of: the
runtime LEA handler is **inlined directly in `run.c` case 10** (lines
220–232), not calling the `exec_lea` function in `opcodes/transfer.h`. The
brief instructs me to modify `exec_lea`, but that function is dead code for
the runtime path — only unit tests exercise it. A minimal Approach-1 fix
must land in `run.c`, not `transfer.h` (or in both, if the unit-test path
is also desired to be exercised). This also has a latent-bug implication
discussed below.

Per the brief's outcome-(d) instruction, the tree is reverted. No code
changes beyond the harness instrumentation added pre-task.

## Evidence

### Phase 1 — reproduction confirmed

```
./harness/harness reference/bios test/images/freedos.img
...
======== HARNESS DIVERGENCE ========
Step             : 65770
Ref CS:IP        : 1FE0:7C65
Our CS:IP        : 1FE0:7C65
Ref opcode bytes : FB 80 7E 24 FF 75 03 88
Our opcode bytes : FB 80 7E 24 FF 75 03 88
Categories       : 0x00000010
  Prefix state:
    seg_override_en: ref=1 ours=0
    seg_override:    ref=12 (→255) ours=1
```

Pre-step (step-limit 65,769):

```
HARNESS_STEP_LIMIT=65769 ./harness/harness ...
harness: reached step limit 65769 with no divergence. Final CS:IP=1FE0:7C62
harness: opcode bytes at CS:IP: 8D 66 A0 FB 80 7E 24 FF
```

Matches the brief: instruction is `8D 66 A0` = `LEA SP, [BP - 0x60]`.

### Phase 2 — both fields already differ; the divergence report shows
it

The comparator's `Prefix state` block (harness.c:521–532) prints *all*
prefix fields whenever the `DIV_PREFIX` bit is set; it does not early-break
per-field. So the existing divergence report already reveals that BOTH
`seg_override_en` and `seg_override` differ:

```
seg_override_en: ref=1 ours=0
seg_override:    ref=12 (→255) ours=1
```

The `(→255)` is the comparator's `ref_to_ours_sreg(12)` — the reference
uses `REG_ZERO = 12` which is outside the 8..11 range of real segment
registers, so the conversion returns `0xFF` ("invalid"). Our emulator's
`seg_override` is `1` (SREG_CS), the residue from whatever segment override
prefix most recently ran before being decremented away.

The `DIV_PREFIX` category only fires here because `seg_override_en != ours`
— the comparator guards the `seg_override` comparison on `ref's
seg_override_en != 0` (harness.c:401–404). Fixing `seg_override_en` on our
side will *unmask* the `seg_override` mismatch rather than close it.

### Phase 3/5 — Approach 1 applied and tested; outcome (d)

I applied the minimal fix the brief recommends, in the runtime LEA (see
"Finding: exec_lea is dead code" below for why the brief's stated location
isn't sufficient):

```c
// packages/emu86/src/emulator/run.c, case 10, LEA branch
} else if (!d->direction) {
    /* LEA (8D) — compute offset without segment */
    if (d->mod < 3) {
        // ... existing offset reconstruction, unchanged ...
    }
    s->seg_override_en = 1;   // <-- EMU-21 side-effect alignment
}
```

Harness run with this fix:

```
======== HARNESS DIVERGENCE ========
Step             : 65770
Ref CS:IP        : 1FE0:7C65
Our CS:IP        : 1FE0:7C65
...
Categories       : 0x00000010
  Prefix state:
    seg_override_en: ref=1 ours=1     <-- closed
    seg_override:    ref=12 (→255) ours=1   <-- still differs
```

`seg_override_en` now matches on both sides (1). But because ref's
`seg_override_en` is 1, the comparator now evaluates the value check, and
`ours=1 != ref_so=0xFF` → `DIV_PREFIX` still fires.

This is Phase 5 **outcome (d)**: fix was correct, but another independent
issue at the same instruction. Per the brief's instruction ("Document what
you found and stop — don't patch it here"), I reverted the fix and am
writing this report.

### Finding: `exec_lea` in `transfer.h` is dead code on the runtime path

While investigating where to apply the fix, I discovered that the runtime
does not call `exec_lea` (the function in `src/emulator/opcodes/transfer.h`
that the brief names). LEA is dispatched from `run.c` case 10 (opcode `8D`)
via an **inlined** implementation at lines 220–232, not via a call to
`exec_lea`.

Concretely, the two copies differ in a substantive way:

| aspect | `exec_lea` in `transfer.h` | inlined in `run.c` case 10 |
|---|---|---|
| when `seg_override_en=0`, default segment for offset reconstruction | hardcoded to `SREG_DS` (line 247) | computed from the decode table (`read_table_sreg(s, seg_reg_idx)`) — this correctly picks SS for `[BP]`-based addressing |
| called from runtime | no (only from unit tests in `test_transfer.c`) | yes |

The hardcoded-DS path in `exec_lea` is a latent bug: for `LEA SP, [BP-0x60]`
(the divergent instruction), the correct default segment is SS, but
`exec_lea` would use DS. It doesn't cause unit-test failures because the
tests in `test_transfer.c` (`lea_basic`, `lea_no_memory_access`) both set
`SREG_DS = 0`, which silently masks the bug.

The correct place for the Approach-1 side-effect fix is `run.c` case 10.
If the EMU-21 unit tests (per Phase 4) are added, they should ideally
test via the runtime path (`emu86_run` stepping through an `8D` opcode)
rather than calling `exec_lea` directly, so they catch both the side
effect and the latent hardcoded-DS bug. Or `exec_lea` should be fixed
and then actually wired into `run.c` case 10 (DRY cleanup), but that's
out of scope for a narrow alignment fix.

## Why the remaining `seg_override` mismatch can't be closed here

The mismatch is a type/semantics gap between the two emulators:

- **Reference** represents segment-override target as a byte-offset into
  `regs16[]`. `REG_ZERO = 12` points at a pseudo-register that holds `0`,
  which is the whole point of 8086tiny's "LEA via override" trick —
  `SEGREG(REG_ZERO, ...)` evaluates to `0*16 + offset = offset`.
- **Ours** represents segment-override target as an SREG enum value
  (0..3 → ES, CS, SS, DS). We have no "zero segment" register. `uint8_t
  seg_override` in `Emu86State` has no encoding that means "use 0 as the
  base".

The comparator's `ref_to_ours_sreg(12)` returns `0xFF` ("invalid") because
12 is outside `[8..11]`. Any value we put in our `seg_override` will be in
`[0..3]`, which never equals `0xFF`.

Options, none of which are in scope per the brief:

1. **Approach 2 from the brief** — add a zero-segment pseudo-register to
   `Emu86State.sregs` (extend to 5 entries), introduce `SREG_ZERO = 4`,
   set `seg_override = SREG_ZERO` in LEA, teach `ref_to_ours_sreg(12)` to
   return 4 instead of 0xFF, update snapshot serialisation. Brief says
   "out of scope".

2. **Comparator equivalence class** — teach `harness/harness.c` that when
   `ref`'s `seg_override == REG_ZERO`, our `seg_override` value is
   "don't care" since nothing will consume it before the next main-loop
   decrement. Small change, single file (`harness.c`). Brief does not
   explicitly list this as in or out of scope. Phase 8 ("Keep the harness
   instrumentation") only covers the opcode-bytes printing addition —
   reading this as "don't otherwise touch harness behaviour", I've
   left it alone.

3. **Sentinel value in our state** — set our `seg_override = 0xFF` in
   LEA, add a guard in `decode.h` and every consumer
   (`transfer.h:92/104/209/287`, `string.h:27`, `run.c:227/634` — at
   least seven sites) to treat 0xFF as "use table default". This is
   structural, not "one or two lines added to exec_lea". Brief says
   "no structural changes".

None fit under the brief's scope. The remaining mismatch is a genuinely
more-interesting alignment issue that deserves its own task.

## Changes made

Pre-task state preserved. The only staged/unstaged modification to `src/`
or `harness/` is the opcode-bytes instrumentation in `harness/harness.c`
(which Phase 8 explicitly mandates keeping — it was already present when
this session started).

```
$ git status
On branch master
Your branch is up to date with 'origin/master'.

Changes not staged for commit:
	modified:   packages/emu86/harness/harness.c       # Phase 8 opcode-bytes printer, pre-existing
Untracked files:
	tasks/emu21-task.md                                # unchanged
	tasks/triage/emu21-triage-report.md                # this report
```

No `exec_lea` edit, no `run.c` edit, no test additions, no quirks-doc
update. The harness still reproduces the original divergence at step
65,770 exactly as the brief's Phase 1 describes.

## Recommended next step

Open **EMU-22: Align LEA's transient `seg_override` value with the
reference**, scoped as a follow-on to the EMU-21 partial alignment:

1. First, land the Approach-1 side-effect fix that EMU-21 identified as
   correct — `s->seg_override_en = 1` at end of LEA in `run.c` case 10.
   This closes the `seg_override_en` field. Also consider either fixing
   `exec_lea` in `transfer.h` to mirror the runtime (use table-derived
   segment) and routing `run.c` case 10 to call it, or deleting `exec_lea`
   and the two unit tests that call it — currently it's dead code.

2. Then pick one of the three options above for the `seg_override` value
   alignment. My weak preference is option 2 (comparator equivalence class
   for REG_ZERO) — it's small, localised to the harness, and preserves the
   "no structural changes to emulator state" principle of recent tasks. It
   does expand the harness beyond pure-structural comparison, but the
   precedent for "equivalence-class comparisons" is already present in
   the category-based field gating (e.g., the flags masker on
   `harness.c:396`), so it's a natural extension. Document the
   equivalence explicitly in `docs/notes/8086tiny-quirks.md`.

3. Add unit tests for LEA's side effect (as EMU-21 Phase 4 describes),
   but via the runtime path (`emu86_run` one step through an `8D` opcode)
   so they catch both the side-effect alignment and the latent `exec_lea`
   hardcoded-DS bug if `exec_lea` is retained.

4. Run the harness past step 65,770. Expect a new divergence at a later
   step; file it as the next task.

5. Regression-test standalone `./emu86 reference/bios test/images/freedos.img`.

## Follow-up concerns

- **Dead-code `exec_lea`.** Independent of EMU-21's alignment question,
  `exec_lea` in `transfer.h` is unreachable from the runtime. Either wire
  it in (DRY-cleanup) or delete it and its two unit tests. If kept but
  unwired, new developers reading the code may assume it's the canonical
  LEA implementation and modify the wrong copy.

- **Unit-test coverage gap.** The existing LEA tests (`lea_basic`,
  `lea_no_memory_access`) both set `SREG_DS = 0`, which masks the
  hardcoded-DS bug in `exec_lea`. A test that sets `SREG_DS != 0` and
  uses `[BP]`-based addressing (defaulting to SS) would expose that bug.
  Worth adding when EMU-22 lands regardless of which option is chosen.

- **Harness comparator's early-break discipline.** `compare_states` sets
  category flags but breaks on the first *GP-register* mismatch within
  `DIV_REGS` (harness.c:384–387) and similarly for memory and I/O
  (harness.c:426, 445). `DIV_PREFIX` is field-granular by design
  (harness.c:521–532 prints all four fields unconditionally), which is
  why we could see both prefix fields differ in one report. If future
  tasks encounter outcome-(d) situations in the GP-register category,
  the comparator would only show the first differing register; a
  rigour-pass on the comparator (report-all-differences-per-category
  mode) might be worth adding as a development aid.

- **The `seg_override_en` decrement ordering.** Our main loop decrements
  *after* decode (run.c:558–565), whereas the reference decrements at
  the *top* of each iteration (8086tiny.c:321–325). This is why
  Approach 1's brief-stated assumption ("nothing consults seg_override
  before decrement") is strictly true for the reference but not for
  us — our decode DOES consult `seg_override` if `seg_override_en`
  is set on entry. Today this doesn't cause a bug because the only
  path that sets `seg_override_en` to a value that persists into the
  next step's decode is the explicit segment-override prefix (which
  sets to 2, is decremented to 1 before the next decode, and consumed
  *correctly* by that next decode). EMU-21's fix as stated would not
  change this (it sets to 1 at end of LEA; decrement happens during
  the next step between decode-and-execute, so the next decode sees
  `seg_override_en == 1` briefly). In practice for the step-65,770
  trace, the next instruction is `FB` (STI, no modrm) so no harm
  occurs — but the ordering invariant is subtle and worth an explicit
  test to prevent regressions. Recommend a test case that runs:
  `LEA reg, [mem]` → segment-override-sensitive instruction immediately
  after, and verifies the override is NOT applied to the second
  instruction.

- **Quirks-doc update.** The `docs/notes/8086tiny-quirks.md` entry that
  EMU-21 Phase 7 prescribes should still be written once EMU-22 lands
  both halves of the alignment. It would be premature to write it based
  only on the EMU-21 partial fix.

## Final note

This is the second task in a row where a narrow fix has uncovered a
second-order alignment issue (EMU-20 was cleaner; EMU-19 was cleaner
still). The pattern of "Intel-behaviourally-equivalent divergences"
the brief describes is real, but the task-authoring assumption that
each one can be closed by one or two lines in a single file is starting
to strain. Specifically, the reference's LEA-via-override trick sets
TWO fields (`seg_override_en`, `seg_override`), and our state vocabulary
cannot represent the second. This is the first divergence where the
two emulators' internal-state shapes are *incompatible* rather than
merely different, which is why it needs a slightly more structural
approach.

EMU-21 as written is closeable — just not in one commit. Recommend
closing it as partial-success-triaged (this report), opening EMU-22
as the narrow continuation that picks up where this left off.
