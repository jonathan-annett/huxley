## Task: EMU-06

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-05: Reference source, core structs, decoder, arithmetic opcodes, logic opcodes. All tests passing.

### Your task

**Goal:** Implement the shift and rotate opcode family: SHL/SAL, SHR, SAR, ROL, ROR, RCL, RCR.

These are used heavily by compilers for multiplication/division by powers of 2, bit field extraction, and multi-precision arithmetic. The flag behaviour, especially for OF, has subtle rules that depend on the shift count.

### The shift/rotate operations

All shift/rotate instructions (opcode group at C0/C1/D0/D1/D2/D3) use the `extra` field from ModRM reg to select the operation:

| extra | Operation |
|-------|-----------|
| 0 | ROL |
| 1 | ROR |
| 2 | RCL |
| 3 | RCR |
| 4 | SHL/SAL |
| 5 | SHR |
| 6 | (undefined, treat as SHL) |
| 7 | SAR |

The shift count comes from one of:
- **1** (opcodes D0/D1 — shift by 1)
- **CL** (opcodes D2/D3 — shift by CL register)
- Note: on the 8086, the full CL value is used. On 80186+, count is masked to 5 bits. We emulate 8086, so use the full value. But shifts of 0 should be a no-op (no flags changed).

**SHL / SAL (Shift Left, extra=4)**
- Shifts bits left, fills LSB with 0
- CF = last bit shifted out (bit 7 for byte, bit 15 for word, before the shift)
- OF = (for count=1 only) CF XOR new MSB. Undefined for count > 1.
- Sets SF, ZF, PF from result. AF undefined.

**SHR (Shift Right Logical, extra=5)**
- Shifts bits right, fills MSB with 0
- CF = last bit shifted out (bit 0 before the shift)
- OF = (for count=1 only) original MSB. Undefined for count > 1.
- Sets SF, ZF, PF from result. AF undefined.

**SAR (Shift Right Arithmetic, extra=7)**
- Shifts bits right, fills MSB with the **original sign bit** (arithmetic shift preserves sign)
- CF = last bit shifted out
- OF = 0 for count=1. Undefined for count > 1.
- Sets SF, ZF, PF from result. AF undefined.

**ROL (Rotate Left, extra=0)**
- Rotates bits left — MSB goes to LSB and also to CF
- CF = new LSB (which was the old MSB)
- OF = (for count=1 only) MSB XOR CF. Undefined for count > 1.
- Does NOT affect SF, ZF, PF, AF.

**ROR (Rotate Right, extra=1)**
- Rotates bits right — LSB goes to MSB and also to CF
- CF = new MSB (which was the old LSB)
- OF = (for count=1 only) MSB XOR (MSB-1). Undefined for count > 1.
- Does NOT affect SF, ZF, PF, AF.

**RCL (Rotate through Carry Left, extra=2)**
- Rotates left through the carry flag. CF becomes new LSB, old MSB becomes new CF. It's a 9-bit (byte) or 17-bit (word) rotation.
- CF = old MSB
- OF = (for count=1 only) MSB XOR CF. Undefined for count > 1.
- Does NOT affect SF, ZF, PF, AF.

**RCR (Rotate through Carry Right, extra=3)**
- Rotates right through carry. CF becomes new MSB, old LSB becomes new CF.
- CF = old LSB
- OF = (for count=1 only) MSB XOR (MSB-1) of result. Undefined for count > 1.
- Does NOT affect SF, ZF, PF, AF.

### Key subtleties

1. **Count of 0 = no-op.** No flags modified, no operation performed.
2. **Rotates do NOT set SF/ZF/PF.** Only shifts do. Rotates only affect CF and OF.
3. **OF is only defined for count=1** for all shift/rotate operations. For count > 1, OF is undefined. Many emulators set it to 0 or leave it unchanged. Follow the original 8086tiny's behaviour where possible.
4. **SAR preserves the sign bit.** After shifting, the MSB remains what it was before. This is how signed division by 2 works.
5. **RCL/RCR are 9-bit/17-bit rotations** including CF. The effective rotation width is `(operand_width_in_bits + 1)`.

### Files to create

**`src/emulator/opcodes/shift.h`**

```c
static inline void exec_shl(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_shr(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_sar(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_rol(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_ror(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_rcl(Emu86State *s, DecodeContext *d, uint8_t count);
static inline void exec_rcr(Emu86State *s, DecodeContext *d, uint8_t count);

// Dispatcher: select operation based on extra field, get count from
// instruction encoding (1 or CL). Called from the main switch.
static inline void exec_shift_rotate(Emu86State *s, DecodeContext *d, uint8_t count);
```

**`test/unit/test_shift.c`**

```
=== SHL ===

TEST: shl_byte_by_1
  - AL=0x80 → AL=0x00, CF=1 (bit 7 shifted out), ZF=1

TEST: shl_byte_by_1_no_carry
  - AL=0x01 → AL=0x02, CF=0

TEST: shl_byte_by_4
  - AL=0x0F → AL=0xF0, CF=0

TEST: shl_word_by_1
  - AX=0x8000 → AX=0x0000, CF=1, ZF=1

TEST: shl_of_set
  - AL=0x40, shift by 1 → AL=0x80, CF=0, OF=1 (CF XOR MSB = 0 XOR 1 = 1)

TEST: shl_of_clear
  - AL=0x80, shift by 1 → AL=0x00, CF=1, OF=1 (CF XOR MSB = 1 XOR 0 = 1)

TEST: shl_sets_szp
  - AL=0x01, shift by 1 → AL=0x02, SF=0, ZF=0, PF=0

TEST: shl_count_zero_noop
  - AL=0x42, flags=known state, shift by 0 → AL=0x42, all flags unchanged

=== SHR ===

TEST: shr_byte_by_1
  - AL=0x01 → AL=0x00, CF=1, ZF=1

TEST: shr_byte_by_1_no_carry
  - AL=0x80 → AL=0x40, CF=0

TEST: shr_byte_by_4
  - AL=0xF0 → AL=0x0F

TEST: shr_word_by_1
  - AX=0x0001 → AX=0x0000, CF=1

TEST: shr_of_set
  - AL=0x80, shift by 1 → OF=1 (original MSB was 1)

TEST: shr_of_clear
  - AL=0x40, shift by 1 → OF=0 (original MSB was 0)

TEST: shr_fills_zero
  - AL=0xFF, shift by 1 → AL=0x7F (MSB is 0)

=== SAR ===

TEST: sar_byte_positive
  - AL=0x40, shift by 1 → AL=0x20 (sign bit 0, fill 0)

TEST: sar_byte_negative
  - AL=0x80, shift by 1 → AL=0xC0 (sign bit 1, fill 1)

TEST: sar_byte_negative_by_4
  - AL=0xF0, shift by 4 → AL=0xFF (sign preserved through all shifts)

TEST: sar_word_negative
  - AX=0x8000, shift by 1 → AX=0xC000

TEST: sar_of_zero_for_count_1
  - SAR always sets OF=0 for count=1

TEST: sar_preserves_sign
  - AL=0xFF, shift by 7 → AL=0xFF (all bits are sign extension)

=== ROL ===

TEST: rol_byte_by_1
  - AL=0x80 → AL=0x01, CF=1 (old bit 7 rotated to bit 0 and CF)

TEST: rol_byte_by_4
  - AL=0x12 → AL=0x21

TEST: rol_word_by_1
  - AX=0x8001 → AX=0x0003, CF=1

TEST: rol_does_not_affect_szp
  - Set ZF=1 before ROL, assert ZF=1 after (rotates don't touch SZP)

TEST: rol_count_zero_noop
  - No flags changed

=== ROR ===

TEST: ror_byte_by_1
  - AL=0x01 → AL=0x80, CF=1

TEST: ror_byte_by_4
  - AL=0x12 → AL=0x21

TEST: ror_word_by_1
  - AX=0x0001 → AX=0x8000, CF=1

TEST: ror_does_not_affect_szp
  - Rotates don't touch SZP flags

=== RCL ===

TEST: rcl_byte_by_1_cf_clear
  - CF=0, AL=0x80 → AL=0x00, CF=1 (old bit 7 out, old CF=0 in at bit 0)

TEST: rcl_byte_by_1_cf_set
  - CF=1, AL=0x00 → AL=0x01, CF=0 (old CF=1 in at bit 0)

TEST: rcl_word_by_1
  - CF=1, AX=0x0000 → AX=0x0001, CF=0

TEST: rcl_9bit_rotation
  - CF=0, AL=0x01, rotate by 9 → should cycle back to start (AL=0x01, CF=0)

=== RCR ===

TEST: rcr_byte_by_1_cf_clear
  - CF=0, AL=0x01 → AL=0x00, CF=1

TEST: rcr_byte_by_1_cf_set
  - CF=1, AL=0x00 → AL=0x80, CF=0 (CF goes into MSB)

TEST: rcr_word_by_1
  - CF=1, AX=0x0000 → AX=0x8000, CF=0
```

### Deliverables
1. `src/emulator/opcodes/shift.h` — All shift/rotate implementations
2. `test/unit/test_shift.c` — All tests listed above, passing
3. Updated Makefile with `test-shift` target
4. All previous tests still pass (`make test-unit`)

### Rules
- All functions `static inline`
- Count of 0 must be a complete no-op — no flags modified
- Rotates (ROL/ROR/RCL/RCR) must NOT affect SF, ZF, PF, AF
- Shifts (SHL/SHR/SAR) DO set SF, ZF, PF
- SAR must fill with the sign bit, not zero
- RCL/RCR rotate through carry — the effective rotation is 9 bits (byte) or 17 bits (word)
- OF is only meaningful for count=1. For count > 1, set OF to 0 or leave unchanged — just be consistent.

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-06
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu06-task.md tasks/completed/
   git add -A
   git commit -m "EMU-06: Shift and rotate opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-06: Shift and rotate opcodes (PARTIAL - see task log)"
