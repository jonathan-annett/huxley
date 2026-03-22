## Task: EMU-04

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01: Reference source analysis
- EMU-02: Core structs (Emu86State, DecodeContext, Emu86Platform, Emu86Tables), snapshot, ring buffers (390 assertions passing)
- EMU-03: Instruction decoder with ModRM, EA calculation, register/memory helpers (460 assertions total, all passing)

### Your task

**Goal:** Implement the arithmetic opcode family: ADD, ADC, SUB, SBB, CMP, NEG, INC, DEC, MUL, IMUL, DIV, IDIV. These are the most frequently executed instructions in any x86 program. Getting them right — especially the flags — is critical.

### Design constraints

1. **All opcode functions are `static inline` in header files.** Create `src/emulator/opcodes/arithmetic.h`. This will be `#include`d by `run.c` (the unity build) later. For now, the test file includes it directly.

2. **Each logical operation gets a named function.** Not one giant switch — individual functions like `exec_add`, `exec_sub`, `exec_mul_byte`, `exec_div_word`, etc. Group related operations but keep them readable.

3. **Flag computation is the hardest part.** The 8086 sets CF, PF, AF, ZF, SF, OF differently for each class of operation. Get this wrong and DOS programs will crash on conditional jumps. Provide flag helper functions:
   - `set_flags_szp(state, result, width)` — set SF, ZF, PF based on result
   - `set_flags_add(state, dest, src, result, width)` — set CF, AF, OF for addition
   - `set_flags_sub(state, dest, src, result, width)` — set CF, AF, OF for subtraction
   - `set_flags_logic(state, result, width)` — clear CF and OF, set SF, ZF, PF (for AND/OR/XOR — used later but define now)

4. **Create a flags helper header: `src/emulator/opcodes/helpers.h`** — this is shared across all opcode families. It contains:
   - Flag get/set helpers: `get_flag(state, FLAG_xx)`, `set_flag(state, FLAG_xx)`, `clear_flag(state, FLAG_xx)`, `update_flag(state, FLAG_xx, value)`
   - The `set_flags_*` functions listed above
   - Parity computation (use a lookup table or compute — the original uses TABLE_PARITY_FLAG)
   - `TOP_BIT(width)` — returns 0x80 for byte, 0x8000 for word
   - `SIGN_OF(value, width)` — extract sign bit
   - `MASK(width)` — returns 0xFF for byte, 0xFFFF for word

5. **Use the `DecodeContext` for operand access.** The decoder (EMU-03) already populated `rm_addr`, `op_to_addr`, `op_from_addr`, etc. The arithmetic functions use `read_rm()`, `write_rm()`, `read_reg()` from `decode.h` to access operands.

6. **Each function takes `(Emu86State *s, DecodeContext *d)` or `(Emu86State *s, DecodeContext *d, Emu86Tables *t)`.** The state is modified (registers, flags, memory). The decode context provides operand information.

### The arithmetic operations

Refer to ORIGINAL-ANALYSIS.md sections B (Macro Inventory) and D (Opcode Dispatch) for how the original implements these.

**ADD (opcode variants: 00-05, 80-83 group extra=0)**
- `dest = dest + src`
- Sets: CF (carry out of MSB), PF, AF (carry out of bit 3), ZF, SF, OF (signed overflow)

**ADC (opcode variants: 10-15, 80-83 group extra=2)**
- `dest = dest + src + CF`
- Same flag behavior as ADD, but includes carry flag in the addition

**SUB (opcode variants: 28-2D, 80-83 group extra=5)**
- `dest = dest - src`
- Sets: CF (borrow), PF, AF (borrow from bit 4), ZF, SF, OF (signed overflow)

**SBB (opcode variants: 18-1D, 80-83 group extra=3)**
- `dest = dest - src - CF`
- Same flag behavior as SUB, but includes carry flag

**CMP (opcode variants: 38-3D, 80-83 group extra=7)**
- Same as SUB but result is discarded — only flags are set
- `temp = dest - src`, set flags, do NOT write result back

**NEG (F6/F7 group, extra=3)**
- `dest = 0 - dest` (two's complement negation)
- CF = 1 unless operand was 0
- OF = 1 if operand was 0x80 (byte) or 0x8000 (word) — the one value that overflows on negation
- Sets PF, AF, ZF, SF normally

**INC (opcode variants: 40-47, FE/FF group extra=0)**
- `dest = dest + 1`
- Sets PF, AF, ZF, SF, OF — but does **NOT** affect CF. This is a critical difference from ADD.

**DEC (opcode variants: 48-4F, FE/FF group extra=1)**
- `dest = dest - 1`
- Sets PF, AF, ZF, SF, OF — but does **NOT** affect CF.

**MUL (F6/F7 group, extra=4)**
- Unsigned multiply
- Byte: AX = AL * src (8-bit), CF=OF=1 if AH != 0
- Word: DX:AX = AX * src (16-bit), CF=OF=1 if DX != 0

**IMUL (F6/F7 group, extra=5)**
- Signed multiply
- Byte: AX = (signed)AL * (signed)src, CF=OF=1 if result doesn't fit in AL (sign-extended)
- Word: DX:AX = (signed)AX * (signed)src, CF=OF=1 if result doesn't fit in AX

**DIV (F6/F7 group, extra=6)**
- Unsigned divide
- Byte: AL = AX / src, AH = AX % src
- Word: AX = DX:AX / src, DX = DX:AX % src
- Divide by zero → INT 0 (set `d->divide_error = 1` or return an error code the caller checks)
- Quotient overflow → INT 0

**IDIV (F6/F7 group, extra=7)**
- Signed divide
- Same register layout as DIV but with signed operands
- Divide by zero → INT 0
- Quotient overflow → INT 0

### Files to create

**`src/emulator/opcodes/helpers.h`** — Flag helpers shared by all opcode families:

```c
// Flag manipulation
static inline int get_flag(const Emu86State *s, uint16_t flag);
static inline void set_flag(Emu86State *s, uint16_t flag);
static inline void clear_flag(Emu86State *s, uint16_t flag);
static inline void update_flag(Emu86State *s, uint16_t flag, int condition);

// Width helpers  
static inline uint16_t MASK(uint8_t width);      // 0xFF or 0xFFFF
static inline uint16_t TOP_BIT(uint8_t width);    // 0x80 or 0x8000
static inline int SIGN_OF(uint32_t val, uint8_t width);

// Parity (even parity of low 8 bits)
static inline int parity8(uint8_t val);

// Flag group setters
static inline void set_flags_szp(Emu86State *s, uint32_t result, uint8_t width);
static inline void set_flags_add(Emu86State *s, uint32_t dest, uint32_t src, uint32_t result, uint8_t width);
static inline void set_flags_sub(Emu86State *s, uint32_t dest, uint32_t src, uint32_t result, uint8_t width);
static inline void set_flags_logic(Emu86State *s, uint32_t result, uint8_t width);
static inline void set_flags_inc(Emu86State *s, uint32_t dest, uint32_t result, uint8_t width);  // no CF
static inline void set_flags_dec(Emu86State *s, uint32_t dest, uint32_t result, uint8_t width);  // no CF
```

Flag details for `set_flags_add`:
- `CF` = carry out of MSB: `result > MASK(width)` or equivalently `(result >> (8*(width+1))) & 1`
- `AF` = carry out of bit 3: `((dest ^ src ^ result) >> 4) & 1`
- `OF` = signed overflow: `((dest ^ result) & (src ^ result) >> (TOP_BIT position)) & 1` — both operands same sign, result different sign

Flag details for `set_flags_sub`:
- `CF` = borrow: `dest < src` (unsigned comparison)
- `AF` = borrow from bit 4: `((dest ^ src ^ result) >> 4) & 1`
- `OF` = signed overflow: `((dest ^ src) & (dest ^ result) >> (TOP_BIT position)) & 1` — operands different sign, result sign differs from dest

**`src/emulator/opcodes/arithmetic.h`** — Arithmetic opcode implementations:

```c
// Core arithmetic operations
static inline void exec_add(Emu86State *s, DecodeContext *d);
static inline void exec_adc(Emu86State *s, DecodeContext *d);
static inline void exec_sub(Emu86State *s, DecodeContext *d);
static inline void exec_sbb(Emu86State *s, DecodeContext *d);
static inline void exec_cmp(Emu86State *s, DecodeContext *d);
static inline void exec_neg(Emu86State *s, DecodeContext *d);
static inline void exec_inc(Emu86State *s, DecodeContext *d);
static inline void exec_dec(Emu86State *s, DecodeContext *d);

// Multiply/divide — these interact with AX/DX directly
static inline void exec_mul(Emu86State *s, DecodeContext *d);
static inline void exec_imul(Emu86State *s, DecodeContext *d);
// Returns 0 on success, 1 if divide error (caller should trigger INT 0)
static inline int exec_div(Emu86State *s, DecodeContext *d);
static inline int exec_idiv(Emu86State *s, DecodeContext *d);

// BCD arithmetic (include here since they're arithmetic-adjacent)
static inline void exec_daa(Emu86State *s);
static inline void exec_das(Emu86State *s);
static inline void exec_aaa(Emu86State *s);
static inline void exec_aas(Emu86State *s);
static inline void exec_aam(Emu86State *s, uint8_t base);  // base is usually 10
static inline void exec_aad(Emu86State *s, uint8_t base);
```

**`test/unit/test_arithmetic.c`** — Comprehensive tests:

```
=== Flag helpers ===

TEST: parity8
  - parity8(0x00) == 1 (even parity — zero 1-bits)
  - parity8(0x01) == 0 (odd parity — one 1-bit)
  - parity8(0x03) == 1 (even — two 1-bits)
  - parity8(0xFF) == 1 (even — eight 1-bits)

TEST: set_flags_szp_byte
  - Result 0x00: ZF=1, SF=0, PF=1
  - Result 0x80: ZF=0, SF=1, PF=0
  - Result 0x01: ZF=0, SF=0, PF=0
  - Result 0xFF: ZF=0, SF=1, PF=1

TEST: set_flags_szp_word
  - Result 0x0000: ZF=1, SF=0, PF=1 (PF checks low 8 bits only)
  - Result 0x8000: ZF=0, SF=1, PF=1
  - Result 0x0100: ZF=0, SF=0, PF=1 (low byte is 0x00, even parity)

=== ADD ===

TEST: add_byte_no_flags
  - AL=0x10, BL=0x20 → AL=0x30, CF=0, ZF=0, SF=0, OF=0

TEST: add_byte_carry
  - AL=0xFF, BL=0x01 → AL=0x00, CF=1, ZF=1

TEST: add_byte_overflow
  - AL=0x7F, BL=0x01 → AL=0x80, OF=1, SF=1 (positive + positive = negative)

TEST: add_byte_auxiliary_carry
  - AL=0x0F, BL=0x01 → AF=1 (carry out of bit 3)

TEST: add_word_basic
  - AX=0x1000, BX=0x2000 → AX=0x3000

TEST: add_word_carry
  - AX=0xFFFF, BX=0x0001 → AX=0x0000, CF=1, ZF=1

TEST: add_word_overflow
  - AX=0x7FFF, BX=0x0001 → AX=0x8000, OF=1

=== ADC ===

TEST: adc_without_carry
  - CF=0, AL=0x10, BL=0x20 → AL=0x30 (same as ADD when CF=0)

TEST: adc_with_carry
  - CF=1, AL=0x10, BL=0x20 → AL=0x31

TEST: adc_carry_propagation
  - CF=1, AL=0xFF, BL=0x00 → AL=0x00, CF=1 (carry in causes carry out)

=== SUB ===

TEST: sub_byte_no_borrow
  - AL=0x30, BL=0x10 → AL=0x20, CF=0

TEST: sub_byte_borrow
  - AL=0x00, BL=0x01 → AL=0xFF, CF=1

TEST: sub_byte_overflow
  - AL=0x80, BL=0x01 → AL=0x7F, OF=1 (negative - positive = positive)

TEST: sub_word_basic
  - AX=0x3000, BX=0x1000 → AX=0x2000

=== SBB ===

TEST: sbb_without_borrow
  - CF=0, AL=0x30, BL=0x10 → AL=0x20

TEST: sbb_with_borrow
  - CF=1, AL=0x30, BL=0x10 → AL=0x1F

=== CMP ===

TEST: cmp_equal
  - AL=0x42, BL=0x42 → ZF=1, CF=0, AL unchanged (still 0x42)

TEST: cmp_less
  - AL=0x10, BL=0x20 → CF=1, ZF=0, AL unchanged

TEST: cmp_greater
  - AL=0x20, BL=0x10 → CF=0, ZF=0, AL unchanged

=== NEG ===

TEST: neg_positive
  - AL=0x01 → AL=0xFF, CF=1

TEST: neg_zero
  - AL=0x00 → AL=0x00, CF=0, ZF=1

TEST: neg_overflow
  - AL=0x80 → AL=0x80, OF=1, CF=1 (the only value where NEG overflows)

TEST: neg_word
  - AX=0x0001 → AX=0xFFFF, CF=1

=== INC / DEC ===

TEST: inc_basic
  - AL=0x41 → AL=0x42

TEST: inc_does_not_affect_carry
  - CF=1, AL=0x41 → AL=0x42, CF still 1

TEST: inc_overflow
  - AL=0x7F → AL=0x80, OF=1

TEST: inc_byte_wrap
  - AL=0xFF → AL=0x00, ZF=1, CF unchanged (this is the key difference from ADD)

TEST: dec_basic
  - AL=0x42 → AL=0x41

TEST: dec_does_not_affect_carry
  - CF=1, AL=0x42 → AL=0x41, CF still 1

TEST: dec_overflow
  - AL=0x80 → AL=0x7F, OF=1

TEST: dec_zero
  - AL=0x01 → AL=0x00, ZF=1

TEST: inc_word
  - AX=0xFFFF → AX=0x0000, ZF=1, CF unchanged

=== MUL ===

TEST: mul_byte_basic
  - AL=3, operand=4 → AX=12, CF=0, OF=0

TEST: mul_byte_high_result
  - AL=0x80, operand=0x02 → AX=0x0100, CF=1, OF=1 (result > 0xFF)

TEST: mul_word_basic
  - AX=100, operand=200 → DX:AX = 20000, CF=0, OF=0

TEST: mul_word_high_result
  - AX=0xFFFF, operand=0xFFFF → DX:AX = 0xFFFE0001, CF=1, OF=1

=== IMUL ===

TEST: imul_byte_basic
  - AL=3, operand=4 → AX=12 (signed, positive * positive)

TEST: imul_byte_negative
  - AL=-2 (0xFE), operand=3 → AX=-6 (0xFFFA), CF=1, OF=1 (doesn't fit in signed byte)

TEST: imul_byte_fits
  - AL=-1 (0xFF), operand=1 → AX=0xFFFF (-1), CF=0, OF=0 (fits in signed byte as -1)

TEST: imul_word_basic
  - AX=10, operand=20 → DX:AX=200

=== DIV ===

TEST: div_byte_basic
  - AX=10, operand=3 → AL=3 (quotient), AH=1 (remainder)

TEST: div_byte_exact
  - AX=12, operand=4 → AL=3, AH=0

TEST: div_byte_by_zero
  - AX=10, operand=0 → returns error (INT 0)

TEST: div_byte_overflow
  - AX=0x0400 (1024), operand=1 → quotient 1024 > 255, returns error (INT 0)

TEST: div_word_basic
  - DX=0, AX=1000, operand=7 → AX=142, DX=6

TEST: div_word_by_zero
  - Returns error

=== IDIV ===

TEST: idiv_byte_basic
  - AX=10, operand=3 → AL=3, AH=1

TEST: idiv_byte_negative_dividend
  - AX=-10 (0xFFF6), operand=3 → AL=-3 (0xFD), AH=-1 (0xFF)

TEST: idiv_byte_negative_divisor
  - AX=10, operand=-3 (0xFD) → AL=-3 (0xFD), AH=1

TEST: idiv_byte_by_zero
  - Returns error

=== BCD ===

TEST: daa_basic
  - AL=0x0A after ADD → AL=0x10, AF=1 (adjust lower nibble)

TEST: daa_both_nibbles
  - AL=0x9A → AL=0x00, CF=1 (both nibbles adjusted)

TEST: das_basic
  - AL=0x10 after SUB that set AF → AL=0x0A

TEST: aaa_basic
  - AL=0x0A → AL=0x00, AH incremented, AF=1, CF=1

TEST: aas_basic
  - AL=0xFF after subtraction → adjust, AH decremented

TEST: aam_basic
  - AL=15 → AH=1, AL=5 (15 / 10 = 1 remainder 5)

TEST: aad_basic
  - AH=1, AL=5 → AL=15 (1 * 10 + 5), AH=0
```

### How to structure the tests

Each test should:
1. Call `emu86_init(&state)` to get a clean state
2. Load the BIOS into memory and call `emu86_load_tables(&tables, &state)` (needed for flag table lookups if you use the parity table)
3. Set up the registers for the test scenario
4. For instructions that use ModRM operands: place the instruction bytes at CS:IP, call `decode_instruction()`, then call the exec function. For simpler tests that just test the arithmetic logic: call the exec function directly with a manually constructed DecodeContext.
5. Assert register values and flag states

Use a helper function to reduce boilerplate:
```c
static void setup_state(Emu86State *s, Emu86Tables *t) {
    emu86_init(s);
    // Load BIOS from reference/bios into mem at F000:0100
    // Call emu86_load_tables(t, s)
}
```

### Deliverables
1. `src/emulator/opcodes/helpers.h` — Flag helpers, width helpers, parity
2. `src/emulator/opcodes/arithmetic.h` — All arithmetic opcode implementations
3. `test/unit/test_arithmetic.c` — All tests listed above, passing
4. Updated Makefile with `test-arithmetic` target
5. All previous tests still pass (`make test-unit`)

### Rules
- All functions must be `static inline`
- Hot-path flag helpers should use `__attribute__((always_inline))`
- INC and DEC must NOT modify CF — this is the most common arithmetic bug in 8086 emulators
- DIV and IDIV must detect divide-by-zero AND quotient overflow — both trigger INT 0
- CMP must NOT write the result back to the destination
- The parity flag is computed on the **low 8 bits only**, even for 16-bit operations
- AF (auxiliary carry) is the carry out of bit 3, used for BCD arithmetic — get this right or DAA/DAS won't work

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-04
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues encountered, design decisions made, or deviations from the task spec}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../                          # repo root
   mv tasks/emu04-task.md tasks/completed/
   git add -A
   git commit -m "EMU-04: Arithmetic opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Still commit and push, but do NOT move the task file to completed/
   - Commit message: "EMU-04: Arithmetic opcodes (PARTIAL - see task log)"
