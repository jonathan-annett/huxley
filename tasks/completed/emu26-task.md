# EMU-26: Align SF/ZF/PF after MUL/IMUL to match 8086tiny reference

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

With EMU-25 landed, the harness now runs deterministically regardless of terminal stdin state. Running the harness to divergence surfaces the next real lockstep issue at step 66,392:

```
Step: 66392
Ref CS:IP: 1FE0:7C9D   (post-step; the divergent instruction was at 7C9A)
Our CS:IP: 1FE0:7C9D   (same)
Opcode bytes at pre-step CS:IP=1FE0:7C9A: F7 66 16 01 C6 11 D7 89
Categories: 0x00000008 (FLAGS only)
FLAGS: ref=0204 ours=0244  xor=0040
  ZF: ref=0 ours=1
```

Same CS:IP on both sides, same opcode bytes, only ZF differs. The instruction at 1FE0:7C9A is:

`F7 66 16` = `MUL word ptr [BP+0x16]` (3 bytes; ModR/M byte 0x66 has mod=01, reg=100 (MUL), rm=110 (BP+disp8), disp8=0x16)

This is another Intel-undefined-flag case, in the same spirit as EMU-20 (AF after shifts). Intel documents that MUL and IMUL set CF and OF based on whether the result overflows the destination register, and leaves SF, ZF, AF, PF undefined. Both "don't touch those flags" (our current behaviour) and "derive them from the full result" (8086tiny's behaviour) are Intel-legal. But they differ, and the harness catches that difference.

### What 8086tiny does

`reference/8086tiny.c:109-113` defines `MUL_MACRO`:

```c
#define MUL_MACRO(op_data_type,out_regs) (set_opcode(0x10), \
    out_regs[i_w + 1] = (op_result = CAST(op_data_type)mem[rm_addr] * (op_data_type)*out_regs) >> 16, \
    regs16[REG_AX] = op_result, \
    set_OF(set_CF(op_result - (op_data_type)op_result)))
```

Key points:
- `op_result` is `int` (32-bit), holds the full product (32 bits for word MUL, 16 bits for byte MUL).
- `set_opcode(0x10)` marks the instruction as "ADC-shaped" for the post-dispatch flags update path.
- CF and OF are set by the final `set_OF(set_CF(...))` based on whether the result has a nonzero high half.
- SF/ZF/PF are NOT set inside the macro. They get set in the post-dispatch flags update block at `8086tiny.c:695-707`:

```c
if (set_flags_type & FLAGS_UPDATE_SZP)
{
    regs8[FLAG_SF] = SIGN_OF(op_result);
    regs8[FLAG_ZF] = !op_result;
    regs8[FLAG_PF] = bios_table_lookup[TABLE_PARITY_FLAG][(unsigned char)op_result];
    ...
}
```

Because `set_opcode(0x10)` selects the ADC table entry (which has `FLAGS_UPDATE_SZP` set), MUL effectively gets SZP-update treatment based on `op_result`.

The specific semantics:
- **ZF**: `!op_result` — 1 iff the FULL product (32 bits for word, 16 for byte) is zero.
- **SF**: `SIGN_OF(op_result)` — bit 15 of low 16 bits for word, bit 7 of low 8 bits for byte. Effectively the sign bit of AX (or AL) after MUL.
- **PF**: parity of low 8 bits (AL after MUL).
- **AF**: not touched by the SZP-update; left at whatever state preceded MUL. (The AF-update is inside `FLAGS_UPDATE_AO_ARITH`, not `FLAGS_UPDATE_SZP`.)

Note the inconsistency, which must be preserved: **ZF looks at the full product, but SF and PF look at truncated halves.** That's how 8086tiny behaves, and matching it exactly is the goal.

### What our emulator does

`src/emulator/opcodes/arithmetic.h` defines `exec_mul` (and `exec_imul`). They set only CF and OF, leaving SF/ZF/PF/AF untouched.

For the step-66,392 case, the pre-MUL state happens to have ZF=1 (e.g. from some earlier zero-result operation). Our MUL doesn't clear it, so ZF stays 1. Reference clears it because the product is nonzero.

### What this task does

Update `exec_mul` and `exec_imul` to set SF/ZF/PF after the multiplication, matching 8086tiny's semantics exactly:
- ZF from the full product (32-bit for word, 16-bit for byte)
- SF from the low half's top bit (bit 15 for word, bit 7 for byte)
- PF from the low 8 bits

Do NOT touch AF — leave it untouched, matching the reference.

Add unit tests that verify ZF/SF/PF align with reference semantics across a range of MUL and IMUL cases, including the specific cases that expose the difference (e.g. product fits in AX vs overflows into DX but low half is zero).

### What this task does NOT do

- DIV/IDIV flag behaviour — Intel-undefined for all flags; reference leaves them in unpredictable state. If the harness surfaces a DIV divergence later, that's a separate task.
- The register-memory aliasing dormant concern.
- Harness structural improvements.
- Any `run.c` changes.
- Any Makefile changes.

## Your task

### Phase 1 — Confirm the current divergence

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected: harness halts at step 66,392 with `ZF: ref=0 ours=1` on `MUL word ptr [BP+0x16]` at CS:IP=1FE0:7C9A.

If you see a different divergence step or different categories, stop and report. EMU-26 assumes the MUL/ZF divergence is present.

### Phase 2 — Read the reference's flag-update logic

Read these specific regions to ground yourself:

- `reference/8086tiny.c:109-113` — MUL_MACRO
- `reference/8086tiny.c:138` — SIGN_OF macro
- `reference/8086tiny.c:695-707` — post-dispatch SZP flag update
- `reference/8086tiny.c:395-398` — how MUL and IMUL dispatch into MUL_MACRO

Confirm your mental model of what ZF/SF/PF should be after MUL and IMUL for both byte and word forms.

### Phase 3 — Update exec_mul and exec_imul

Edit `src/emulator/opcodes/arithmetic.h`. Find `exec_mul` (around line 184) and `exec_imul` (around line 213).

For both functions, after computing the result and setting CF/OF, add SF/ZF/PF updates.

**For word operations** (`d->operand_width` is true):
- ZF = (full 32-bit result == 0)
- SF = bit 15 of the low 16 bits of result, i.e., bit 15 of `(uint16_t)result`, i.e., the sign bit of AX after MUL
- PF = parity of low 8 bits of result, i.e., `(uint8_t)result`

**For byte operations**:
- ZF = (full 16-bit result == 0)
- SF = bit 7 of `(uint8_t)result`, i.e., sign bit of AL after MUL
- PF = parity of `(uint8_t)result`

For IMUL, use the underlying unsigned bit pattern for the ZF/SF/PF computations — the reference's `SIGN_OF` casts via `CAST(short)` which is identical to reinterpreting the low 16 bits as signed. So `result == 0` semantics apply to the raw bit pattern regardless of signedness, and SF is still just bit 15/7 of the low half.

**Parity calculation.** The reference uses a BIOS table lookup. We can use a direct computation. A common pattern:

```c
static inline int parity8(uint8_t b) {
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return !(b & 1);  /* 8086 parity is EVEN-parity: PF=1 if low 8 bits have even number of 1s */
}
```

Check whether the codebase already has a parity helper — if yes, use that one; if no, add the helper in a suitable place (probably `state.h` or a new `flags.h` if there's no good spot). Don't scatter ad-hoc parity calculations.

**AF is NOT to be touched.** Leave it at whatever state preceded the MUL. The reference explicitly does not update AF inside the SZP block.

### Phase 4 — Unit tests

Add tests in `test/unit/test_arithmetic.c` (or wherever the existing MUL/IMUL tests live — grep for `exec_mul` in `test/` to locate).

Tests to add (or extend existing ones):

**For MUL:**
1. `MUL word: 0 * 0 = 0 → ZF=1, SF=0, PF=1, CF=0, OF=0`
2. `MUL word: 1 * 1 = 1 → ZF=0, SF=0, PF=0, CF=0, OF=0` (parity of 1 is odd → PF=0)
3. `MUL word: 0xFFFF * 2 = 0x1FFFE → AX=FFFE, DX=0001 → ZF=0, SF=1, PF=0 (parity of 0xFE = odd), CF=1, OF=1`
4. `MUL word: 0x8000 * 2 = 0x10000 → AX=0000, DX=0001 → ZF=0 (full product nonzero!), SF=0, PF=1, CF=1, OF=1`  **← key test, exposes the bug**
5. `MUL byte: 0xFF * 0x02 = 0x01FE → AX=01FE → ZF=0, SF=1 (bit 7 of low byte=1), PF=0, CF=1, OF=1`
6. `MUL byte: 0x10 * 0x10 = 0x0100 → AX=0100 → ZF=0 (full product nonzero), SF=0, PF=1 (AL=0 → even parity), CF=1, OF=1`  **← key test**

**For IMUL:**
1. `IMUL word: 0 * -1 = 0 → ZF=1, SF=0, PF=1`
2. `IMUL word: -1 * -1 = 1 → ZF=0, SF=0, PF=0`
3. `IMUL byte: -128 * 2 = -256 → AX=FF00 → ZF=0, SF=0 (bit 7 of low byte=0), PF=1`

**Regression guard:** pre-set a non-zero / non-matching value in SF/ZF/PF before the MUL/IMUL, run the instruction, and verify the flags are now what the new code computes — not what they were before. This catches "forgot to update" regressions.

Follow the existing test style in `test/unit/test_arithmetic.c` — use the same assertion helpers, fixture patterns, naming conventions as neighboring tests.

**Important:** after adding tests, also verify **existing** MUL/IMUL tests still pass. If any fail, that's a signal that the previous tests had assumptions about "SF/ZF/PF are left untouched by MUL" — update those tests to reflect the new, reference-aligned behaviour. Note any such updates in the commit message.

### Phase 5 — Verify

```
cd packages/emu86
make test-arithmetic
./test/unit/test_arithmetic
```

Expected: all tests pass, including new ones.

Then the standalone emulator:

```
make emu86
./emu86 reference/bios test/images/freedos.img
```

Expected: reaches FreeDOS banner. (Divide-by-zero at banner is a known separate issue, unrelated.)

Then the harness:

```
touch src/emulator/run.c && make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -15
```

The `touch src/emulator/run.c` is the known workaround for the Makefile-header-dependency quirk — changes to opcode headers don't trigger harness rebuild without it.

**Expected outcome:** harness advances PAST step 66,392. The next divergence (if any) is at some later step. Report the new divergence step, CS:IP, and category. That's EMU-27 territory, not this task's problem.

### Phase 6 — Full test suite

```
cd packages/emu86
make test
```

Expected: all tests pass.

### Phase 7 — Commit

Commit if:
- Phase 1 confirmed the MUL/ZF divergence.
- Phase 4 tests all pass.
- Phase 5 harness advances past step 66,392.
- Phase 6 full test suite is green.
- `git status` shows only:
  - `src/emulator/opcodes/arithmetic.h` modified (MUL/IMUL flag updates)
  - `test/unit/test_arithmetic.c` modified (new tests)
  - Possibly `src/emulator/state.h` or a new `src/emulator/opcodes/flags.h` if you added a parity helper
  - `tasks/emu26-task.md` created
  - No other emulator or harness files changed
  - No binaries
  - No changes to `reference/`, `bios.asm`, or any EMU-25 artifacts

Commit message:

```
EMU-26: Set SF/ZF/PF after MUL/IMUL to match reference

The 8086 manual marks SF, ZF, AF, PF as undefined after MUL/IMUL;
only CF and OF have defined semantics. Both "leave undefined flags
alone" (our previous behaviour) and "derive them from the full
result" (8086tiny) are Intel-legal, but they differ, and the
differential harness caught the mismatch at step 66,392 on a
MUL word ptr [BP+0x16] instruction where the full product was
nonzero but happened to have a zero low half — our ZF stayed 1
(left from before), reference cleared ZF to 0.

Aligning to 8086tiny's semantics:
- ZF from the FULL product (32-bit for word MUL, 16-bit for byte)
- SF from bit 15 of low 16 bits (word) / bit 7 of low 8 bits (byte)
- PF from parity of low 8 bits
- AF unchanged (reference does not touch AF in the SZP-update path)

Scope:
- exec_mul and exec_imul in arithmetic.h updated
- New unit tests cover ZF from full product (the bug), plus
  SF/PF correctness across byte/word MUL/IMUL
- No run.c changes
- No Makefile changes

Verification:
- test-arithmetic passes including regression-guard cases
- Full make test green
- Standalone emu86 reaches FreeDOS banner
- Harness advances past step 66,392 (next divergence at step N
  is EMU-27)

Follow-up:
- EMU-27: whatever the harness finds after 66,392
```

Task log entry:

```
## EMU-26
Date: {today}
Status: PASS
Test results: N → N+K (K new MUL/IMUL flag tests)
Harness: advances past step 66,392; next divergence at step {N}
Notes: Aligned SF/ZF/PF-after-MUL/IMUL to reference semantics.
Intel-undefined flags, similar shape to EMU-20's AF-on-shifts.
Preserved the reference's quirk where ZF looks at the full product
while SF/PF look at low halves.
```

Then:

```
mv tasks/emu26-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 8 — Report (on failure)

Triage to `tasks/triage/emu26-triage-report.md` if:
- Phase 1 doesn't show the expected MUL/ZF divergence (premise falsified).
- Phase 4 tests reveal my specifications above are wrong (e.g., reference's PF or SF actually computed differently than I described).
- Phase 5 harness regresses or fails to advance.
- Phase 6 shows unrelated test failures (investigate whether caused by this change or pre-existing).

In the triage report, include the specific test output and enough context for the next iteration to be drafted precisely.

## Out of scope — do not touch

- DIV/IDIV flag behaviour.
- Any Makefile changes (including the header-dependency quirk).
- Harness infrastructure.
- Any non-MUL/IMUL arithmetic instructions.
- The dormant items: editor-api-proposal, latent 0xEA length bug, 0xC0/0xC1 reference bug, silent-exit-on-0:0, register-memory aliasing, FreeDOS divide-by-zero.
- EMU-25's source-patching machinery (it's stable; leave it).

## Final note

The failure mode to guard against: **forgetting to test that previous flag values are overwritten.** If your test only checks "after MUL with expected product, flags have correct values," you might pass even with code that computes the flags correctly only when they start zeroed. The regression-guard test (Phase 4, last item) specifically sets non-matching flags before the MUL and verifies the new code overwrites them. Without that test, a future change that accidentally makes the flag updates conditional on some pre-state could pass all other checks and reintroduce the bug.

Related failure mode: **mixing up which half determines which flag.** The reference's inconsistency (ZF from full result, SF/PF from low half) is deliberate — or at least, deliberately replicated from what real 8086 silicon happened to do. If you find yourself writing "ZF = (low_half == 0)" you've made the wrong choice; re-read the reference's line 699 carefully.
