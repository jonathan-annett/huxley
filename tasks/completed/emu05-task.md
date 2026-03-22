## Task: EMU-05

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01: Reference source analysis
- EMU-02: Core structs, snapshot, ring buffers (390 assertions)
- EMU-03: Instruction decoder (460 assertions total)
- EMU-04: Arithmetic opcodes with flag helpers (618 assertions total)

### Your task

**Goal:** Implement the logic opcode family: AND, OR, XOR, NOT, TEST.

These are simpler than arithmetic because the flag rules are uniform: all logic operations clear CF and OF, and set SF, ZF, PF based on the result. The `set_flags_logic()` helper from EMU-04's `helpers.h` already does this.

### The logic operations

**AND (opcode variants: 20-25, 80-83 group extra=4)**
- `dest = dest & src`
- Flags: CF=0, OF=0, SF/ZF/PF from result. AF is undefined (set to 0 for predictability).

**OR (opcode variants: 08-0D, 80-83 group extra=1)**
- `dest = dest | src`
- Flags: same as AND

**XOR (opcode variants: 30-35, 80-83 group extra=6)**
- `dest = dest ^ src`
- Flags: same as AND
- Note: `XOR reg, reg` is the standard way to zero a register. ZF=1, SF=0, PF=1 after this.

**NOT (F6/F7 group, extra=2)**
- `dest = ~dest` (bitwise complement)
- Flags: **NO flags affected.** This is the only ALU-style instruction that doesn't touch flags.

**TEST (opcode variants: 84-85 for reg,rm; A8-A9 for AL/AX,imm; F6/F7 group extra=0)**
- `temp = dest & src`, set flags, discard result (like CMP is to SUB, TEST is to AND)
- Flags: same as AND (CF=0, OF=0, SF/ZF/PF from result)

### Files to create

**`src/emulator/opcodes/logic.h`**

```c
static inline void exec_and(Emu86State *s, DecodeContext *d);
static inline void exec_or(Emu86State *s, DecodeContext *d);
static inline void exec_xor(Emu86State *s, DecodeContext *d);
static inline void exec_not(Emu86State *s, DecodeContext *d);
static inline void exec_test(Emu86State *s, DecodeContext *d);
```

**`test/unit/test_logic.c`**

```
TEST: and_byte_basic
  - AL=0xFF, BL=0x0F → AL=0x0F, CF=0, OF=0

TEST: and_byte_zero
  - AL=0xAA, BL=0x55 → AL=0x00, ZF=1

TEST: and_word_basic
  - AX=0xFF00, BX=0x0FF0 → AX=0x0F00

TEST: and_clears_cf_of
  - Set CF=1, OF=1 before AND
  - After AND: CF=0, OF=0

TEST: or_byte_basic
  - AL=0xF0, BL=0x0F → AL=0xFF

TEST: or_byte_zero
  - AL=0x00, BL=0x00 → AL=0x00, ZF=1

TEST: or_word_basic
  - AX=0xFF00, BX=0x00FF → AX=0xFFFF, SF=1

TEST: or_clears_cf_of
  - Set CF=1, OF=1 before OR
  - After OR: CF=0, OF=0

TEST: xor_byte_basic
  - AL=0xFF, BL=0x0F → AL=0xF0

TEST: xor_self_zeros
  - AX=0x1234, XOR AX,AX → AX=0x0000, ZF=1, SF=0, PF=1, CF=0, OF=0

TEST: xor_word_basic
  - AX=0xAAAA, BX=0x5555 → AX=0xFFFF, SF=1

TEST: not_byte_basic
  - AL=0x00 → AL=0xFF

TEST: not_byte_ff
  - AL=0xFF → AL=0x00

TEST: not_word_basic
  - AX=0xAAAA → AX=0x5555

TEST: not_preserves_flags
  - Set CF=1, ZF=1, SF=1, OF=1
  - Execute NOT
  - Assert all flags unchanged

TEST: test_byte_match
  - AL=0xFF, BL=0x0F → ZF=0, AL still 0xFF (result not stored)

TEST: test_byte_no_match
  - AL=0xF0, BL=0x0F → ZF=1, AL still 0xF0

TEST: test_preserves_operands
  - AL=0x42, BL=0x42 → both unchanged after TEST

TEST: test_clears_cf_of
  - Set CF=1, OF=1, execute TEST → CF=0, OF=0

TEST: test_word_basic
  - AX=0x8000, BX=0x8000 → ZF=0, SF=1 (bit 15 set in result)

TEST: test_parity
  - AL=0x03, BL=0xFF → result=0x03, PF=1 (even number of bits)
  - AL=0x01, BL=0xFF → result=0x01, PF=0 (odd number of bits)
```

### Deliverables
1. `src/emulator/opcodes/logic.h` — All logic opcode implementations
2. `test/unit/test_logic.c` — All tests listed above, passing
3. Updated Makefile with `test-logic` target
4. All previous tests still pass (`make test-unit`)

### Rules
- All functions must be `static inline`
- NOT must not modify any flags — this is the key gotcha
- TEST must not write the result back to the destination (like CMP for subtraction)
- Logic ops must clear CF and OF (not just leave them unchanged — explicitly clear)
- AF is technically undefined for logic ops; set it to 0 for predictable behaviour

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-05
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues encountered, design decisions made, or deviations from the task spec}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu05-task.md tasks/completed/
   git add -A
   git commit -m "EMU-05: Logic opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-05: Logic opcodes (PARTIAL - see task log)"
