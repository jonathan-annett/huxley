## Task: EMU-03

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01: Reference source acquired, original source analysis complete
- EMU-02: Core state structs (Emu86State, DecodeContext, Emu86Platform, Emu86Tables), snapshot save/restore, ring buffers. All tests passing (390 assertions).

### Your task

**Goal:** Implement the instruction decoder — the logic that fetches an instruction from memory at CS:IP, decodes the opcode byte, extracts ModRM fields, resolves effective addresses, and populates a `DecodeContext` struct. This is the front half of `execute_instruction()` — decode only, no execution.

This is the most architecturally critical code in the emulator. The original 8086tiny does this through a dense set of macros (`DECODE_RM_REG`, `GET_REG_ADDR`, `SEGREG`) and the `bios_table_lookup` system. We replace all of that with readable, named, `static inline` functions.

### Design constraints

1. **All decode functions are `static inline` in `decode.h`.** This file will be `#include`d by `run.c` (the unity build translation unit). The compiler must have full visibility to inline everything.

2. **The decoder populates a `DecodeContext` (from EMU-02) entirely from `Emu86State` and `Emu86Tables`.** It reads memory at CS:IP, reads the lookup tables, and fills in the decode context. It does NOT modify the state (IP advancement happens later, after execution).

3. **Effective address calculation must exactly match 8086 behaviour.** The 8086 has specific addressing modes (BX+SI, BX+DI, BP+SI, BP+DI, SI, DI, BP/direct, BX) with specific default segment registers. Use the `Emu86Tables` data (tables 0–7) for this, exactly as the original does, since those tables encode the correct mappings.

4. **Segment overrides must be respected.** If `state->seg_override_en > 0`, the override segment replaces the default segment for memory operands.

5. **Helper functions for reading registers and memory must be provided.** These will be used by both the decoder and the opcode implementations:
   - `read_reg8(state, reg_index)` / `write_reg8(state, reg_index, value)`
   - `read_reg16(state, reg_index)` / `write_reg16(state, reg_index, value)` 
   - `read_sreg(state, sreg_index)` / `write_sreg(state, sreg_index, value)`
   - `mem_read8(state, linear_addr)` / `mem_write8(state, linear_addr, value)`
   - `mem_read16(state, linear_addr)` / `mem_write16(state, linear_addr, value)`
   - `segoff_to_linear(segment, offset)` — compute 16*segment + offset

   The 8-bit register mapping is tricky: indices 0-3 map to AL,CL,DL,BL (low bytes of AX,CX,DX,BX) and indices 4-7 map to AH,CH,DH,BH (high bytes). The helpers must handle this correctly.

6. **Use `__attribute__((always_inline))` on the hottest helpers** (memory read/write, register access, segment calculation) as belt-and-suspenders.

### What the decoder does, step by step

Refer to Section D of `docs/ORIGINAL-ANALYSIS.md` (Main Loop Analysis, Phase 1) for the original's decode flow. Our decoder does the same thing in readable functions:

1. **Fetch the opcode byte** from `state->mem` at the linear address `16 * state->sregs[SREG_CS] + state->ip`.

2. **Translate the opcode** using `tables->data[TABLE_XLAT_OPCODE][raw_opcode]` to get `xlat_id` (the translated opcode used in the execution switch). Also extract:
   - `extra` from `TABLE_XLAT_SUBFUNCTION`
   - `set_flags_type` from `TABLE_STD_FLAGS`  
   - `has_modrm` from `TABLE_I_MOD_SIZE`

3. **Extract encoding bits** from the raw opcode:
   - `operand_width` (i_w): bit 0 of raw opcode (for most instructions)
   - `direction` (i_d): bit 1 of raw opcode
   - `reg4bit`: low 3 bits of raw opcode (for register-encoded instructions like MOV reg,imm)

4. **Read data bytes** following the opcode:
   - `i_data0`: byte at opcode+1 (sign-extended to 16-bit for certain uses)
   - `i_data1`: byte at opcode+2
   - `i_data2`: byte at opcode+3

5. **If `has_modrm`**, decode the ModRM byte (which is `i_data0`):
   - Extract `mod` (bits 7-6), `reg` (bits 5-3), `rm` (bits 2-0)
   - Shift `i_data1`/`i_data2` forward based on displacement size
   - **Resolve the effective address** (`rm_addr`) using the addressing mode tables:
     - For `mod < 3` (memory operand): compute the linear address from base register + index register + displacement, with the appropriate segment
     - For `mod == 3` (register operand): `rm_addr` is a register index
   - Set `op_to_addr` and `op_from_addr` based on `direction`

6. **Compute `inst_length`** — the total number of bytes consumed by this instruction, using the same formula as the original:
   ```
   inst_length = base_size + modrm_displacement + immediate_size
   ```
   Where:
   - `base_size` from `TABLE_BASE_INST_SIZE[raw_opcode]`
   - `modrm_displacement` depends on `mod` field (0, 1, or 2 bytes, plus the special case of mod=0,rm=6 which is 2 bytes)
   - `immediate_size` from `TABLE_I_W_SIZE[raw_opcode] * (operand_width + 1)`

### Files to create/modify

**`src/emulator/decode.h`** — Replace the existing stub with the full implementation. Keep the `DecodeContext` struct from EMU-02 and add:

```c
// --- Memory access helpers (always_inline) ---
static inline __attribute__((always_inline))
uint32_t segoff_to_linear(uint16_t segment, uint16_t offset);

static inline __attribute__((always_inline))
uint8_t mem_read8(const Emu86State *s, uint32_t addr);

static inline __attribute__((always_inline))
void mem_write8(Emu86State *s, uint32_t addr, uint8_t val);

static inline __attribute__((always_inline))
uint16_t mem_read16(const Emu86State *s, uint32_t addr);

static inline __attribute__((always_inline))
void mem_write16(Emu86State *s, uint32_t addr, uint16_t val);

// --- Register access helpers ---
// 8-bit: indices 0-3 = AL,CL,DL,BL (low), 4-7 = AH,CH,DH,BH (high)
static inline __attribute__((always_inline))
uint8_t read_reg8(const Emu86State *s, uint8_t index);

static inline __attribute__((always_inline))
void write_reg8(Emu86State *s, uint8_t index, uint8_t val);

static inline __attribute__((always_inline))
uint16_t read_reg16(const Emu86State *s, uint8_t index);

static inline __attribute__((always_inline))
void write_reg16(Emu86State *s, uint8_t index, uint16_t val);

static inline __attribute__((always_inline))
uint16_t read_sreg(const Emu86State *s, uint8_t index);

static inline __attribute__((always_inline))
void write_sreg(Emu86State *s, uint8_t index, uint16_t val);

// --- Operand access (uses decode context) ---
// Read/write the r/m operand (memory or register depending on mod)
static inline uint16_t read_rm16(const Emu86State *s, const DecodeContext *d);
static inline uint8_t read_rm8(const Emu86State *s, const DecodeContext *d);
static inline void write_rm16(Emu86State *s, const DecodeContext *d, uint16_t val);
static inline void write_rm8(Emu86State *s, const DecodeContext *d, uint8_t val);

// Generic read/write that dispatches on operand_width
static inline uint16_t read_rm(const Emu86State *s, const DecodeContext *d);
static inline void write_rm(Emu86State *s, const DecodeContext *d, uint16_t val);
static inline uint16_t read_reg(const Emu86State *s, const DecodeContext *d);
static inline void write_reg(Emu86State *s, const DecodeContext *d, uint16_t val);

// --- The main decode function ---
// Populates DecodeContext from the instruction at CS:IP.
// Does NOT advance IP or modify state.
static inline void decode_instruction(
    const Emu86State *state,
    const Emu86Tables *tables,
    DecodeContext *d
);
```

**`test/unit/test_decode.c`** — Decoder tests:

```
TEST: decode_nop
  - Place NOP (0x90) at CS:IP
  - Decode
  - Assert xlat_id matches expected (check TABLE_XLAT_OPCODE for 0x90)
  - Assert has_modrm == 0
  - Assert inst_length == 1

TEST: decode_mov_ax_imm16
  - Place MOV AX, 0x1234 (bytes: B8 34 12) at CS:IP
  - Decode
  - Assert operand_width == 1 (word)
  - Assert inst_length == 3
  - Assert immediate data available (i_data0/i_data1 contain 0x34, 0x12)

TEST: decode_mov_al_imm8
  - Place MOV AL, 0x42 (bytes: B0 42) at CS:IP
  - Decode  
  - Assert operand_width == 0 (byte)
  - Assert inst_length == 2

TEST: decode_add_rm_reg_direct
  - Place ADD [BX+SI], AX (bytes: 01 00, mod=00 reg=AX rm=BX+SI) at CS:IP
  - Decode
  - Assert has_modrm == 1
  - Assert mod == 0, reg == 0, rm == 0
  - Assert rm_addr is a memory address (not register)
  - Assert direction == 0 (rm ← reg)
  - Assert inst_length == 2

TEST: decode_add_rm_reg_with_disp8
  - Place ADD [BX+SI+0x10], AX (mod=01, disp8=0x10) at CS:IP
  - Decode
  - Assert mod == 1
  - Assert displacement used
  - Assert inst_length == 3

TEST: decode_add_rm_reg_with_disp16
  - Place ADD [BX+SI+0x1234], AX (mod=10, disp16) at CS:IP
  - Decode
  - Assert mod == 2
  - Assert inst_length == 4

TEST: decode_modrm_register_mode
  - Place ADD AX, BX (mod=11, reg=AX, rm=BX) at CS:IP
  - Decode
  - Assert mod == 3
  - Assert rm_addr refers to a register, not memory
  - Assert inst_length == 2

TEST: decode_modrm_direct_address
  - Place MOV AX, [0x1234] (mod=00, rm=110, disp16) — the special case
  - Decode
  - Assert rm_addr == DS:0x1234 (linear address)
  - Assert inst_length includes the 2-byte displacement

TEST: decode_segment_override
  - Set state->seg_override_en = 2, state->seg_override = SREG_ES
  - Place an instruction with a memory operand at CS:IP
  - Decode
  - Assert the effective address uses ES instead of the default segment

TEST: decode_reg8_mapping
  - Verify read_reg8 index mapping:
    - Index 0 → AL (low byte of AX)
    - Index 1 → CL (low byte of CX)
    - Index 4 → AH (high byte of AX)
    - Index 5 → CH (high byte of CX)
  - Set AX = 0xAABB
  - Assert read_reg8(state, 0) == 0xBB (AL)
  - Assert read_reg8(state, 4) == 0xAA (AH)

TEST: write_reg8_mapping
  - Set AX = 0x0000
  - write_reg8(state, 0, 0x42) → assert AX == 0x0042 (AL written)
  - write_reg8(state, 4, 0x99) → assert AX == 0x9942 (AH written, AL preserved)

TEST: segoff_to_linear
  - Assert segoff_to_linear(0xF000, 0x0100) == 0xF0100
  - Assert segoff_to_linear(0x0000, 0x0000) == 0x00000
  - Assert segoff_to_linear(0xFFFF, 0x000F) == 0xFFFF0 + 0xF == 0xFFFFF (wraps within 20-bit space)
  - Assert segoff_to_linear(0x1000, 0x0001) == 0x10001

TEST: mem_read_write_8
  - Write 0x42 to address 0x100
  - Read back, assert 0x42

TEST: mem_read_write_16
  - Write 0x1234 to address 0x100
  - Read back as uint16, assert 0x1234
  - Read individual bytes: addr 0x100 == 0x34 (low), addr 0x101 == 0x12 (high)

TEST: inst_length_prefix_instructions
  - Place segment override prefix (e.g. 0x26 = ES:) followed by an instruction
  - Decode the prefix
  - Assert inst_length == 1

TEST: inst_length_group_opcodes
  - Place an 0x80 group opcode (ALU r/m, imm8) with ModRM
  - Decode, verify inst_length accounts for ModRM + displacement + immediate
```

**Update `Makefile`:**
- Add `test-decode` target that compiles and runs `test/unit/test_decode.c`
- Update `test-unit` to include decode tests
- The decode test needs to link against snapshot.c (for state init) and needs access to the BIOS binary to load tables. The test should either:
  - Load the real BIOS from `reference/bios` and call `emu86_load_tables()`, OR
  - Manually populate the specific table entries needed for the tests

The first approach (loading real BIOS) is preferred because it validates that `emu86_load_tables()` works correctly and tests against real decode table data.

### Deliverables
1. `src/emulator/decode.h` — Full decoder implementation with all helpers (static inline)
2. `test/unit/test_decode.c` — All tests listed above, passing
3. Updated Makefile with `test-decode` target
4. All existing tests still pass (`make test-unit`)
5. Committed from repo root: "EMU-03: Instruction decoder"

### Rules
- All functions in decode.h must be `static inline`
- Hot-path helpers (mem_read/write, reg read/write, segoff_to_linear) must use `__attribute__((always_inline))`
- The decoder must NOT modify `Emu86State` — it only reads state and populates DecodeContext
- The 8-bit register index mapping (0-3 = low byte, 4-7 = high byte) must be correct — this is a common source of bugs
- Test against the real BIOS tables where possible
- Run `make test-unit` (all tests, including previous EMU-02 tests) before committing

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-03
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues encountered, design decisions made, or deviations from the task spec}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../                          # repo root
   mv tasks/emu03-task.md tasks/completed/
   git add -A
   git commit -m "EMU-03: Instruction decoder"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Still commit and push, but do NOT move the task file to completed/
   - Commit message: "EMU-03: Instruction decoder (PARTIAL - see task log)"
