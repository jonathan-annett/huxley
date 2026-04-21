# EMU-29: Set OF for all shift/rotate counts, not just count=1

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

With EMU-28's pre-step snapshot landed, the harness now shows both pre-step and post-step state in divergence reports. The current divergence at step 70,424 is fully characterised:

```
Pre-step CS:IP   : 1FE0:7D1B
Opcode bytes     : D3 E8 ...         (SHR AX, CL)
AX               : FFF0
CX               : 0004              (so CL = 4)
Pre-step FLAGS   : 0215 (OF=0)

Post-step FLAGS  : ref=0A14 (OF=1), ours=0214 (OF=0)
```

The divergent instruction is `SHR AX, CL` with CL=4 and AX=0xFFF0. Both sides produce the same shifted result in AX. The only difference is OF.

### Intel vs reference vs ours

Intel's documented behaviour for shifts and rotates:
- **Count = 0**: no flags change.
- **Count = 1**: OF is defined per instruction (SHL/SHR/SAR/ROL/ROR/RCL/RCR each have a specific formula).
- **Count > 1**: OF is **undefined**.

8086tiny doesn't respect the "undefined" clause. For all shifts and rotates, it applies the count=1 formula unconditionally, regardless of count. See `reference/8086tiny.c:485-509`:

```c
// SHL: always runs, regardless of count
OPCODE 4: set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))))

// SHR: always runs
OPCODE 5: set_OF(SIGN_OF(op_dest))

// SAR: always clears OF
OPCODE 7: set_OF(0); ...
```

The whole block is guarded by `if (scratch_uint)` (line 470) — so count=0 skips everything. But count=2, 3, 4 all go through the OF update.

Our emulator guards the OF update with `if (count == 1)`. For CL=4 today, we skip the update, and OF retains its pre-shift value (0). The reference applies its formula and gets OF=1 (from AX=0xFFF0's MSB).

### Structurally the same pattern as EMU-20/26/27

- EMU-20: AF after shifts — reference leaves alone, we were clearing.
- EMU-26: SF/ZF/PF after MUL — reference sets from op_result, we weren't.
- EMU-27: AF after logical ops — reference leaves alone, we were clearing.
- EMU-29: OF after shifts (count != 1) — reference sets from formula, we weren't.

Intel-undefined flag, specific reference behaviour, differential harness catches the mismatch. Fix is always alignment with the reference.

### Which instructions are affected

Currently, five shift/rotate handlers in `src/emulator/opcodes/shift.h` guard OF with `if (count == 1)`:

- `exec_shl` (~line 30)
- `exec_shr` (~line 52)
- `exec_sar` (~line 98)
- `exec_rol` (~line 126)
- `exec_ror` (~line 153)

`exec_rcl` (~line 185) and `exec_rcr` (~line 216) already set OF unconditionally. They need no change.

### The formulas themselves

The count=1 formulas in our code are correct per Intel and match the reference's formulas. No formula changes are needed — just removing the conditional guard so they apply for all nonzero counts.

- SHL: OF = (new MSB) XOR (last bit shifted out) = `SIGN_OF(result, w) ^ CF`
- SHR: OF = original MSB = `SIGN_OF(val, w)` (val is the pre-shift value)
- SAR: OF = 0 (always, per Intel)
- ROL: OF = (new MSB) XOR CF = `SIGN_OF(result, w) ^ CF`
- ROR: OF = (MSB) XOR (MSB-1) = `SIGN_OF(result, w) ^ ((result >> (bits-2)) & 1)`

All formulas already exist in the code; we just remove the guards.

### What this task does NOT do

- Does not touch the formulas themselves.
- Does not touch RCL or RCR (already correct).
- Does not touch AF behaviour — that was EMU-20.
- Does not touch SF/ZF/PF behaviour — those are set via `set_flags_szp` already.
- No `run.c` changes, no Makefile changes, no harness changes.

## Your task

### Phase 1 — Confirm the current divergence

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected: divergence at step 70,424 with pre-step block showing CS:IP=1FE0:7D1B, opcode bytes starting `D3 E8`, AX=FFF0, CX=0004. Post-step FLAGS differ only in bit 11 (OF).

If you see a different divergence or different flags, stop and report.

### Phase 2 — Review the five handlers

Read `src/emulator/opcodes/shift.h`. Find these five handlers and their `if (count == 1)` OF-update lines:
- `exec_shl` (around line 15-35)
- `exec_shr` (around line 44-62)
- `exec_sar` (around line 71-103)
- `exec_rol` (around line 112-130)
- `exec_ror` (around line 139-157)

Also confirm `exec_rcl` (~line 165-188) and `exec_rcr` (~line 197-219) update OF unconditionally — no change needed for those.

### Phase 3 — Remove the guards

For each of the five handlers, change:

```c
if (count == 1)
    update_flag(s, FLAG_OF, <formula>);
```

to:

```c
update_flag(s, FLAG_OF, <formula>);
```

Formula unchanged. Only the guard is removed.

Update the per-function doc comment to reflect the change. For instance, `exec_shr`'s comment from:

```c
/* SHR — Shift Right Logical
 * CF = last bit shifted out. OF (count=1) = original MSB.
 * Sets SF, ZF, PF.
 */
```

to something like:

```c
/* SHR — Shift Right Logical
 * CF = last bit shifted out. OF = original MSB (applied for all
 * nonzero counts; Intel defines OF only for count=1 but 8086tiny
 * applies the formula unconditionally — see EMU-29).
 * Sets SF, ZF, PF.
 */
```

Similar updates for SHL, SAR, ROL, ROR.

### Phase 4 — Verify formula correctness vs reference

Cross-reference each formula against `reference/8086tiny.c:485-509`:

- **SHL** (reference line 502): `set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))))`.
  - CF = bit shifted out (reference: `SIGN_OF(op_dest << (count-1))` = bit at position (bits-count) of op_dest).
  - OF = new MSB XOR CF.
  - Our formula: `SIGN_OF(result, w) ^ CF`. Equivalent.

- **SHR** (reference line 504): `set_OF(SIGN_OF(op_dest))`.
  - OF = MSB of pre-shift value.
  - Our formula: `SIGN_OF(val, w)` where val is pre-shift. Equivalent.

- **SAR** (reference line 507): `set_OF(0)`.
  - Always 0.
  - Ours: `update_flag(s, FLAG_OF, 0)`. Equivalent.

- **ROL** (reference line 489): `set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1))`.
  - CF = result & 1 (bit rotated out from top, now at position 0).
  - OF = new MSB XOR CF.
  - Our formula: `SIGN_OF(result, w) ^ CF`. Equivalent.

- **ROR** (reference line 493): `set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)))`.
  - CF = MSB of result (bit rotated out from bottom, now at position bits-1).
  - `SIGN_OF(op_result * 2)` = bit (bits-2) of op_result.
  - OF = bit(bits-2) XOR MSB.
  - Our formula: `SIGN_OF(result, w) ^ ((result >> (bits - 2)) & 1)` = MSB XOR bit(bits-2). Same.

If any formula appears to differ materially after this cross-check, stop and report.

### Phase 5 — Unit tests

Find existing shift/rotate tests (grep `exec_shr` or `test_shift` in `test/unit/`). Add OF tests for count != 1.

Required test cases (at minimum):

**SHR:**
1. `SHR word, count=4, AX=0xFFF0` → OF=1 (matches today's divergence exactly).
2. `SHR word, count=4, AX=0x7FFF` → OF=0 (MSB of pre-shift value was 0).
3. `SHR byte, count=3, AL=0x80` → OF=1 (MSB=1).
4. Regression guard: pre-set OF=1, run `SHR word, count=2, AX=0x0001` → OF=0 (MSB=0 overrides pre-state).

**SHL:**
1. `SHL word, count=2, AX=0x4000` → result=0x0000, CF=1 (bit 14 was 1), OF = 0 XOR 1 = 1.
2. `SHL word, count=3, AX=0x2000` → result=0x0000, CF=1 (bit 13 was 1), OF = 0 XOR 1 = 1.
3. `SHL byte, count=2, AL=0x40` → result=0x00, CF=1 (bit 6 was 1), OF = 0 XOR 1 = 1.
4. Regression guard: pre-set OF=0, run something producing OF=1, verify overwrite.

**SAR:**
1. `SAR word, count=2, AX=0x8000` → OF=0 (always).
2. `SAR byte, count=3, AL=0x80` → OF=0.
3. Regression guard: pre-set OF=1, any SAR with count=2, verify OF=0.

**ROL:**
1. `ROL word, count=2` with a predictable value — compute expected new MSB and CF, assert OF = new MSB XOR CF.
2. Regression guard: pre-set wrong OF, verify overwrite.

**ROR:**
1. `ROR word, count=2` with a predictable value — assert OF = MSB XOR (bit bits-2).
2. Regression guard.

Follow existing test style and use existing assertion helpers.

If any existing multi-count shift tests asserted "OF unchanged for count > 1", that assertion is now wrong. Update those tests and note the update in the commit message. Existing tests that only exercised count=1 should pass unchanged.

### Phase 6 — Verify

```
cd packages/emu86
make test-shift
./test/unit/test_shift
```

All tests pass, including new ones.

```
make emu86
./emu86 reference/bios test/images/freedos.img
```

Reaches FreeDOS banner.

```
touch src/emulator/run.c && make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -25
```

Harness advances past step 70,424. Report the new divergence's step, pre-step CS:IP, pre-step opcode bytes, and categories. That's EMU-30.

### Phase 7 — Full test suite

```
make test
```

All tests pass.

### Phase 8 — Commit

Commit if:
- Phase 1 confirmed the SHR/OF divergence at step 70,424.
- Phase 4 confirmed formulas match reference (no formula changes needed).
- Phase 5 tests pass.
- Phase 6 harness advances past step 70,424.
- Phase 7 full test suite is green.
- `git status` shows only:
  - `src/emulator/opcodes/shift.h` modified
  - `test/unit/test_shift.c` modified
  - `tasks/emu29-task.md` created
  - No other files changed
  - No binaries

Commit message:

```
EMU-29: Set OF unconditionally for shift/rotate (SHL/SHR/SAR/ROL/ROR)

Intel defines OF for shifts/rotates only for count=1; for count>1
OF is undefined. Our handlers guarded the OF update with
"if (count == 1)", leaving OF at its pre-shift value for larger
counts. 8086tiny (our oracle) applies the count=1 formula
unconditionally for all nonzero counts. The differential harness
caught the mismatch at step 70,424 on SHR AX, CL with CL=4 and
AX=0xFFF0.

The fix is to drop the "if (count == 1)" guards in exec_shl,
exec_shr, exec_sar, exec_rol, exec_ror. The formulas themselves
are unchanged and already match the reference's formulas for the
count=1 case. exec_rcl and exec_rcr already had no guard.

Fourth Intel-undefined-flag alignment task (after EMU-20, EMU-26,
EMU-27).

Scope:
- src/emulator/opcodes/shift.h: 5 guard-removals; doc comments
  updated for each handler.
- test/unit/test_shift.c: new tests for OF with count >= 2,
  plus regression guards that verify OF overwrite.

Verification:
- test-shift passes
- Full make test green
- Standalone ./emu86 reaches FreeDOS banner
- Harness advances past step 70,424 (next divergence at step {N}
  is EMU-30)
```

Task log entry:

```
## EMU-29
Date: {today}
Status: PASS
Test results: N → N+K (OF-for-count>1 tests across 5 shift/rotate ops)
Harness: advances past step 70,424; next divergence at step {N}
Notes: Dropped "if count == 1" guards around OF updates in SHL,
SHR, SAR, ROL, ROR. Formulas unchanged — they were already
correct for count=1. Fourth Intel-undefined-flag alignment.
```

Then:

```
mv tasks/emu29-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 9 — Report (on failure)

Triage to `tasks/triage/emu29-triage-report.md` if:
- Phase 1 shows a different divergence.
- Phase 4 reveals formula differences.
- Phase 6 harness doesn't advance (other OF-related bugs remain).
- Phase 7 shows unrelated test failures.

## Out of scope — do not touch

- AF, SF, ZF, PF behaviour for shifts.
- Instructions outside shift/rotate.
- Formulas themselves.
- RCL/RCR.
- Any run.c, Makefile, or harness changes.
- Dormant concerns from prior tasks.

## Final note

Failure modes to watch for:

1. **Mistaking a formula difference for a guard difference.** Phase 4 cross-checks each formula against the reference. If any formula appears to differ materially, don't patch it — stop and report. This brief covers guard removal only.

2. **Forgetting SAR.** SAR's formula is trivial (`OF = 0`), easy to skip mentally. It still needs the guard removed — currently our SAR skips the OF=0 set for count>1, leaving OF at its pre-shift value.

3. **Tests that over-specify OF for count>1.** If there are multi-count shift tests asserting "OF unchanged for count>1", those tests are now wrong. Update them. Don't keep a test that validates the OLD behaviour as a regression guard for the NEW behaviour.

The pattern (Intel-undefined, reference has specific behaviour, align) is now firm enough to merit a docs/notes entry. Don't write it as part of EMU-29; just flag in the commit message. A later task will consolidate.
