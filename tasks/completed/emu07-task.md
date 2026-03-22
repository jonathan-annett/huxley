## Task: EMU-07

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-06: Reference source, core structs, decoder, arithmetic, logic, shift/rotate opcodes. All tests passing.

### Your task

**Goal:** Implement the data transfer opcode family: MOV, PUSH, POP, XCHG, LEA, LDS, LES, CBW, CWD, XLAT, LAHF, SAHF, PUSHF, POPF.

These instructions move data between registers, memory, and the stack. Most do not affect flags at all — the exceptions are SAHF (which explicitly loads flags), POPF (which pops flags from stack), and the segment-load instructions which have no flag effects.

### The data transfer operations

**MOV — the most common instruction**
Many encoding variants, all do the same thing: `dest = src`, no flags affected.
- MOV r/m, reg (88-89)
- MOV reg, r/m (8A-8B)
- MOV r/m, imm (C6-C7)
- MOV reg, imm (B0-BF)
- MOV AL/AX, moffs (A0-A1) — direct memory address
- MOV moffs, AL/AX (A2-A3)
- MOV sreg, r/m (8E)
- MOV r/m, sreg (8C)

**PUSH (50-57 for regs, FF/6 for r/m, 06/0E/16/1E for segment regs)**
- Decrements SP by 2, writes 16-bit value to SS:SP
- No flags affected
- Note: PUSH SP — the 8086 pushes the value of SP *after* decrementing. This differs from later processors.

**POP (58-5F for regs, 8F/0 for r/m, 07/17/1F for segment regs)**
- Reads 16-bit value from SS:SP, increments SP by 2
- No flags affected
- Note: POP CS (0x0F) is not valid on 8086 — this opcode is repurposed for the emulator's custom BIOS calls. Don't implement POP CS.

**XCHG (86-87 for r/m,reg; 90-97 for AX,reg)**
- Swaps two operands: `temp = dest; dest = src; src = temp`
- No flags affected
- XCHG AX,AX (opcode 90) is NOP

**LEA (8D) — Load Effective Address**
- `reg = effective address of r/m operand` (the address itself, not the value at that address)
- No flags affected
- Uses the ModRM decode but only takes the computed address, not the memory content

**LDS (C5) — Load pointer into DS:reg**
- Reads a 32-bit far pointer from memory: `reg = [mem], DS = [mem+2]`
- No flags affected

**LES (C4) — Load pointer into ES:reg**
- Reads a 32-bit far pointer from memory: `reg = [mem], ES = [mem+2]`
- No flags affected

**CBW (98) — Convert Byte to Word**
- Sign-extends AL into AX: if AL bit 7 = 1, AH = 0xFF; else AH = 0x00
- No flags affected

**CWD (99) — Convert Word to Doubleword**
- Sign-extends AX into DX:AX: if AX bit 15 = 1, DX = 0xFFFF; else DX = 0x0000
- No flags affected

**XLAT (D7) — Table Lookup**
- `AL = mem[DS:BX + unsigned AL]`
- No flags affected

**LAHF (9F) — Load AH from Flags**
- `AH = low byte of FLAGS` (SF, ZF, AF, PF, CF — bits 7,6,4,2,0)
- No flags affected (we're reading them, not setting them)

**SAHF (9E) — Store AH into Flags**
- Low byte of FLAGS = AH (sets SF, ZF, AF, PF, CF from AH)
- DOES affect flags — it's explicitly loading them

**PUSHF (9C) — Push FLAGS**
- Push the FLAGS register onto the stack
- No flags affected

**POPF (9D) — Pop FLAGS**
- Pop FLAGS from the stack
- ALL flags affected (loaded from stack value)

### Stack helper functions

Create push/pop helpers that the stack instructions and CALL/RET (EMU-09) will share:

```c
// Push a 16-bit value onto the stack
static inline void stack_push(Emu86State *s, uint16_t value);

// Pop a 16-bit value from the stack  
static inline uint16_t stack_pop(Emu86State *s);
```

`stack_push`: SP -= 2, then write value to `mem[SS:SP]` (little-endian).
`stack_pop`: read value from `mem[SS:SP]`, then SP += 2.

Also create a FLAGS pack/unpack helper:

```c
// Pack individual flag bits into a FLAGS word (for PUSHF)
// The FLAGS register in Emu86State is already packed, so this may just return state->flags
// with reserved bits set correctly (bit 1 always 1 on 8086)
static inline uint16_t flags_pack(const Emu86State *s);

// Unpack a FLAGS word into state->flags (for POPF, SAHF)
static inline void flags_unpack(Emu86State *s, uint16_t flags);
```

On the 8086, bit 1 of FLAGS is always 1, and bits 12-15 are always 1 in the FLAGS word. Make sure `flags_pack` sets these correctly.

### Files to create

**`src/emulator/opcodes/transfer.h`**

```c
// Stack operations
static inline void stack_push(Emu86State *s, uint16_t value);
static inline uint16_t stack_pop(Emu86State *s);

// FLAGS pack/unpack
static inline uint16_t flags_pack(const Emu86State *s);
static inline void flags_unpack(Emu86State *s, uint16_t val);

// MOV variants
static inline void exec_mov_rm_reg(Emu86State *s, DecodeContext *d);
static inline void exec_mov_reg_imm(Emu86State *s, DecodeContext *d);
static inline void exec_mov_rm_imm(Emu86State *s, DecodeContext *d);
static inline void exec_mov_al_moffs(Emu86State *s, DecodeContext *d);
static inline void exec_mov_moffs_al(Emu86State *s, DecodeContext *d);
static inline void exec_mov_sreg_rm(Emu86State *s, DecodeContext *d);
static inline void exec_mov_rm_sreg(Emu86State *s, DecodeContext *d);

// Stack
static inline void exec_push_reg(Emu86State *s, uint8_t reg);
static inline void exec_pop_reg(Emu86State *s, uint8_t reg);
static inline void exec_push_rm(Emu86State *s, DecodeContext *d);
static inline void exec_pop_rm(Emu86State *s, DecodeContext *d);
static inline void exec_push_sreg(Emu86State *s, uint8_t sreg);
static inline void exec_pop_sreg(Emu86State *s, uint8_t sreg);
static inline void exec_pushf(Emu86State *s);
static inline void exec_popf(Emu86State *s);

// Exchange
static inline void exec_xchg(Emu86State *s, DecodeContext *d);
static inline void exec_xchg_ax_reg(Emu86State *s, uint8_t reg);

// Address/pointer loads
static inline void exec_lea(Emu86State *s, DecodeContext *d);
static inline void exec_lds(Emu86State *s, DecodeContext *d);
static inline void exec_les(Emu86State *s, DecodeContext *d);

// Conversion
static inline void exec_cbw(Emu86State *s);
static inline void exec_cwd(Emu86State *s);
static inline void exec_xlat(Emu86State *s);

// Flags
static inline void exec_lahf(Emu86State *s);
static inline void exec_sahf(Emu86State *s);
```

**`test/unit/test_transfer.c`**

```
=== Stack helpers ===

TEST: stack_push_pop_roundtrip
  - Set SP=0xFFFE, SS=0x1000
  - Push 0x1234
  - Assert SP == 0xFFFC
  - Assert mem at SS:SP contains 0x34 (low), 0x12 (high)
  - Pop → assert value == 0x1234
  - Assert SP == 0xFFFE (restored)

TEST: stack_push_multiple
  - Push 0xAAAA, then 0xBBBB
  - Pop → 0xBBBB (LIFO)
  - Pop → 0xAAAA

TEST: stack_sp_wraps
  - SP=0x0000, push → SP=0xFFFE (wraps in 16-bit)

=== MOV ===

TEST: mov_reg_imm_byte
  - MOV AL, 0x42 → AL=0x42

TEST: mov_reg_imm_word
  - MOV AX, 0x1234 → AX=0x1234

TEST: mov_no_flags
  - Set known flags, execute MOV, assert all flags unchanged

TEST: mov_reg_to_mem
  - AX=0x5678, MOV [addr], AX → mem at addr contains 0x5678

TEST: mov_mem_to_reg
  - Write 0xABCD to memory, MOV AX, [addr] → AX=0xABCD

TEST: mov_sreg
  - MOV DS, AX (AX=0x2000) → DS=0x2000
  - MOV AX, DS (DS=0x3000) → AX=0x3000

=== PUSH / POP ===

TEST: push_pop_reg
  - AX=0x1234, PUSH AX, set AX=0, POP AX → AX=0x1234

TEST: push_pop_sreg
  - DS=0x2000, PUSH DS, DS=0, POP DS → DS=0x2000

TEST: pushf_popf_roundtrip
  - Set specific flags (CF=1, ZF=1, SF=0, OF=1)
  - PUSHF
  - Clear all flags
  - POPF
  - Assert CF=1, ZF=1, SF=0, OF=1

TEST: pushf_reserved_bits
  - PUSHF → check pushed value has bit 1 set (always 1 on 8086)

=== XCHG ===

TEST: xchg_reg_reg
  - AX=0x1111, BX=0x2222, XCHG AX,BX → AX=0x2222, BX=0x1111

TEST: xchg_reg_mem
  - AX=0xAAAA, mem=0xBBBB → AX=0xBBBB, mem=0xAAAA

TEST: xchg_no_flags
  - Set flags, XCHG, assert unchanged

TEST: xchg_ax_ax_is_nop
  - AX=0x1234, XCHG AX,AX → AX=0x1234

=== LEA ===

TEST: lea_basic
  - LEA AX, [BX+SI+0x10] with BX=0x100, SI=0x200
  - AX = 0x310 (the address, NOT the memory content)

TEST: lea_no_memory_access
  - Fill memory at the computed address with 0xFF
  - LEA loads the address, not 0xFF

=== LDS / LES ===

TEST: lds_basic
  - Store far pointer at memory: offset=0x1234, segment=0x5678
  - LDS BX, [addr] → BX=0x1234, DS=0x5678

TEST: les_basic
  - Store far pointer at memory: offset=0xAAAA, segment=0xBBBB
  - LES BX, [addr] → BX=0xAAAA, ES=0xBBBB

=== CBW / CWD ===

TEST: cbw_positive
  - AL=0x42 → AH=0x00 (positive, zero-extend)

TEST: cbw_negative
  - AL=0x80 → AH=0xFF (negative, sign-extend)

TEST: cwd_positive
  - AX=0x1234 → DX=0x0000

TEST: cwd_negative
  - AX=0x8000 → DX=0xFFFF

=== XLAT ===

TEST: xlat_basic
  - BX=0x200 (table base), AL=0x05, mem[DS:0x205]=0x42
  - XLAT → AL=0x42

=== LAHF / SAHF ===

TEST: lahf_basic
  - Set CF=1, PF=1, ZF=1, SF=0
  - LAHF → AH = flags low byte (bits 0,2,4,6,7 mapped)

TEST: sahf_basic
  - AH = 0xD5 (SF=1, ZF=1, AF=0, PF=1, CF=1)
  - SAHF → flags updated accordingly

TEST: sahf_does_not_affect_of
  - OF=1, SAHF with data that doesn't include OF
  - Assert OF unchanged (SAHF only affects low 8 bits of flags)
```

### Deliverables
1. `src/emulator/opcodes/transfer.h` — All data transfer implementations
2. `test/unit/test_transfer.c` — All tests listed above, passing
3. Updated Makefile with `test-transfer` target
4. All previous tests still pass (`make test-unit`)

### Rules
- All functions `static inline`
- MOV, PUSH, POP, XCHG, LEA, LDS, LES, CBW, CWD, XLAT, LAHF, PUSHF do NOT affect flags
- SAHF affects only SF, ZF, AF, PF, CF (the low byte of FLAGS) — NOT OF, DF, IF, TF
- POPF affects ALL flags
- PUSHF must set bit 1 to 1 (8086 convention) and bits 12-15 to 1
- `stack_push` and `stack_pop` will be reused by CALL/RET/INT/IRET in EMU-09 — design them as standalone helpers
- PUSH SP on 8086 pushes the decremented value (SP after the push, not before)
- LEA computes the address, it does NOT read memory at that address

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-07
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu07-task.md tasks/completed/
   git add -A
   git commit -m "EMU-07: Data transfer opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-07: Data transfer opcodes (PARTIAL - see task log)"
