# 8086tiny Original Source Analysis

Comprehensive analysis of `reference/8086tiny.c` (Revision 1.25) by Adrian Cable.

---

## A. Global Variables Inventory

### CPU State (belongs in snapshot)

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `regs8` | `unsigned char*` | Pointer to 8-bit register file (aliases into `mem` at `REGS_BASE` / 0xF0000) | No (derived from `mem`) |
| `regs16` | `unsigned short*` | Pointer to 16-bit register file (aliases into `mem` at `REGS_BASE`) | No (derived from `mem`) |
| `reg_ip` | `unsigned short` | Instruction pointer (IP) | **Yes** |
| `trap_flag` | `unsigned char` | Latched trap flag — fires INT 1 on next instruction | **Yes** |

Note: The actual register values (AX, BX, CX, DX, SP, BP, SI, DI, CS, DS, ES, SS, flags) are stored in `mem[0xF0000..0xF001F]` via the `regs8`/`regs16` pointers, and individual flag bytes at `mem[REGS_BASE + FLAG_CF..FLAG_OF]`. They are part of the `mem` array, not separate variables.

### Machine State (belongs in snapshot)

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `mem` | `unsigned char[0x10FFF0]` | Entire emulated memory space (~1.06 MB). Includes RAM, video RAM, ROM area, and memory-mapped registers at 0xF0000. | **Yes** |
| `io_ports` | `unsigned char[0x10000]` | I/O port space (64K ports) | **Yes** |
| `disk` | `int[3]` | File descriptors: disk[0]=HD, disk[1]=FD, disk[2]=BIOS | **Yes** (as disk state, not raw fd) |
| `inst_counter` | `unsigned int` | Instruction counter — used for timer/keyboard/video polling intervals | **Yes** |
| `int8_asap` | `unsigned char` | Flag: pending timer interrupt (INT 8/0xA) | **Yes** |
| `spkr_en` | `unsigned char` | Speaker enable state (bits 0-1 of port 0x61) | **Yes** |
| `wave_counter` | `unsigned short` | Audio waveform position counter | **Yes** |
| `io_hi_lo` | `unsigned char` | PIT programming byte select (alternates high/low byte) | **Yes** |
| `GRAPHICS_X` | `unsigned int` | Current graphics resolution X (set via Hercules reprogramming) | **Yes** |
| `GRAPHICS_Y` | `unsigned int` | Current graphics resolution Y (set via Hercules reprogramming) | **Yes** |
| `file_index` | `unsigned short` | Used during init for disk open loop; reused nowhere after | No (init only) |

### Instruction Decode State (belongs in snapshot — active mid-instruction)

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `opcode_stream` | `unsigned char*` | Pointer into `mem` at current CS:IP | No (derived) |
| `raw_opcode_id` | `unsigned char` | Raw opcode byte | Transient |
| `xlat_opcode_id` | `unsigned char` | Translated opcode (index into switch) | Transient |
| `extra` | `unsigned char` | Sub-function index from TABLE_XLAT_SUBFUNCTION | Transient |
| `i_w` | `unsigned char` | Word flag: 0 = byte operation, 1 = word operation | Transient |
| `i_d` | `unsigned char` | Direction flag from instruction encoding | Transient |
| `i_reg` | `unsigned char` | REG field from ModRM byte | Transient |
| `i_rm` | `unsigned char` | R/M field from ModRM byte | Transient |
| `i_mod` | `unsigned char` | MOD field from ModRM byte | Transient |
| `i_mod_size` | `unsigned char` | Whether instruction uses ModRM (0 or 1) | Transient |
| `i_reg4bit` | `unsigned char` | Low 3 bits of raw opcode (register encoding) | Transient |
| `i_data0` | `unsigned int` | Instruction byte at offset +1 (sign-extended to short) | Transient |
| `i_data1` | `unsigned int` | Instruction byte at offset +2 (sign-extended to short) | Transient |
| `i_data2` | `unsigned int` | Instruction byte at offset +3 (or +4 depending on addressing) | Transient |
| `seg_override_en` | `unsigned char` | Segment override active countdown (2→1→0) | **Yes** (spans instructions) |
| `seg_override` | `unsigned short` | Which segment register is overriding | **Yes** (spans instructions) |
| `rep_override_en` | `unsigned char` | REP prefix active countdown (2→1→0) | **Yes** (spans instructions) |
| `rep_mode` | `unsigned char` | REP mode: 0 = REPNZ, 1 = REPZ | **Yes** (spans instructions) |
| `set_flags_type` | `unsigned int` | Bitfield controlling which flags to update after execution | Transient |

### Scratch/Temporary (NOT in snapshot)

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `op_source` | `unsigned int` | Source operand value for current operation | No |
| `op_dest` | `unsigned int` | Destination operand value (before operation) | No |
| `op_result` | `int` | Result of current operation (signed for flag computation) | No |
| `op_to_addr` | `unsigned int` | Address (offset into `mem`) of destination operand | No |
| `op_from_addr` | `unsigned int` | Address (offset into `mem`) of source operand | No |
| `rm_addr` | `unsigned int` | Effective address from ModRM decode | No |
| `scratch_uint` | `unsigned int` | General-purpose scratch | No |
| `scratch2_uint` | `unsigned int` | General-purpose scratch | No |
| `scratch_int` | `int` | Signed scratch (used in DIV) | No |
| `scratch_uchar` | `unsigned char` | Scratch byte (used in conditional jumps) | No |
| `clock_buf` | `time_t` | Scratch for RTC time | No |
| `ms_clock` | `struct timeb` | Scratch for millisecond clock | No |
| `vmem_ctr` | `unsigned int` | Declared but appears unused in source | No |

### SDL/Graphics State (NO_GRAPHICS excludes these)

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `sdl_screen` | `SDL_Surface*` | SDL display surface | No (runtime only) |
| `sdl_event` | `SDL_Event` | SDL event buffer | No |
| `sdl_audio` | `SDL_AudioSpec` | Audio configuration | No |
| `vid_mem_base` | `unsigned char*` | Pointer to start of video RAM in `mem` | No (derived) |
| `vid_addr_lookup` | `unsigned short[0x10000]` | Precomputed video address translation table | No (derived) |
| `pixel_colors` | `unsigned int[16]` | Precomputed CGA/Hercules palette | No (derived) |
| `cga_colors` | `unsigned short[4]` | CGA color constants | No (constant) |

### Lookup Tables

| Name | Type | Description | Snapshot? |
|------|------|-------------|-----------|
| `bios_table_lookup` | `unsigned char[20][256]` | Instruction decoding tables loaded from BIOS image at startup | **Yes** (or re-derive from BIOS) |

---

## B. Macro Inventory

### Instruction Decode Macros

#### `DECODE_RM_REG`
**What it does:** Decodes the ModRM byte fields (i_mod, i_rm, i_reg) into effective addresses. Computes `rm_addr` (memory/register address for R/M operand) and `op_from_addr` / `op_to_addr` based on the direction bit `i_d`. Uses `bios_table_lookup` tables 0–3 to resolve base registers for each addressing mode.

**Used by:** Nearly every instruction that has a ModRM byte — opcodes 2, 5, 7–12, 15, 17, 18, 24, 37, and more.

**Reads:** `i_mod`, `i_rm`, `i_reg`, `i_d`, `i_data1`, `seg_override_en`, `seg_override`, `regs16[]`, `bios_table_lookup[0..3][]`, `i_w`
**Writes:** `op_to_addr`, `op_from_addr`, `rm_addr`, `scratch_uint`, `scratch2_uint`

#### `GET_REG_ADDR(reg_id)`
**What it does:** Returns offset into `mem` array for register `reg_id`. For 16-bit (`i_w=1`): `REGS_BASE + 2*reg_id`. For 8-bit (`i_w=0`): maps AL,CL,DL,BL to low bytes and AH,CH,DH,BH to high bytes using `2*reg_id + reg_id/4 & 7`.

**Used by:** `DECODE_RM_REG`, opcodes 1, 2, 16

**Reads:** `i_w`
**Writes:** Nothing (expression macro)

#### `SEGREG(reg_seg, reg_ofs, op)`
**What it does:** Converts segment:offset to linear address: `16 * regs16[reg_seg] + (unsigned short)(op regs16[reg_ofs])`. The `op` parameter allows inline pre/post increment/decrement of the offset register.

**Used by:** `DECODE_RM_REG`, `R_M_PUSH`, `R_M_POP`, string ops (17, 18), `pc_interrupt`, opcodes 44, 48, and more.

**Reads:** `regs16[]`
**Writes:** May modify `regs16[reg_ofs]` via the `op` parameter

### Arithmetic/Logic Macros

#### `R_M_OP(dest, op, src)`
**What it does:** Executes `dest op src` with proper 8/16-bit casting based on `i_w`. Stores the old destination in `op_dest` and result in `op_result`, source in `op_source`. This is the core ALU operation macro.

**Used by:** Almost every arithmetic/logic instruction.

**Reads:** `i_w`
**Writes:** `op_dest`, `op_result`, `op_source`, and the destination operand

#### `MEM_OP(dest, op, src)`
**What it does:** `R_M_OP(mem[dest], op, mem[src])` — operates on memory-mapped values.

**Used by:** Most instructions via `OP()`.

#### `OP(op)`
**What it does:** `MEM_OP(op_to_addr, op, op_from_addr)` — executes operation between the decoded destination and source.

**Used by:** ADD, OR, ADC, SBB, AND, SUB, XOR, CMP, MOV, NOT, NEG, XCHG, and more.

**Reads:** `op_to_addr`, `op_from_addr`

#### `MUL_MACRO(op_data_type, out_regs)`
**What it does:** Implements MUL/IMUL. Multiplies `mem[rm_addr]` by accumulator, stores 32-bit result split across DX:AX (or AH:AL for byte). Sets CF/OF if upper half is non-zero.

**Used by:** Opcodes 4 (MUL) and 5 (IMUL) within opcode group 6.

**Reads:** `rm_addr`, `i_w`, `mem[]`, `regs16[]`/`regs8[]`
**Writes:** `op_result`, `regs16[REG_AX]`, `out_regs[i_w+1]`, CF, OF

#### `DIV_MACRO(out_data_type, in_data_type, out_regs)`
**What it does:** Implements DIV/IDIV. Divides DX:AX (or AX for byte) by `mem[rm_addr]`. Stores quotient in AL/AX, remainder in AH/DX. Fires INT 0 on divide-by-zero or overflow.

**Used by:** Opcodes 6 (DIV) and 7 (IDIV) within opcode group 6.

**Reads:** `rm_addr`, `i_w`, `mem[]`, `regs16[]`
**Writes:** `scratch_int`, `scratch_uint`, `scratch2_uint`, quotient/remainder registers

#### `DAA_DAS(op1, op2, mask, min)`
**What it does:** Implements DAA (Decimal Adjust after Addition) and DAS (Decimal Adjust after Subtraction). Adjusts AL for BCD arithmetic.

**Used by:** Opcode 28 (DAA/DAS).

**Reads:** `regs8[REG_AL]`, `regs8[FLAG_AF]`, `regs8[FLAG_CF]`
**Writes:** `op_result`, `scratch2_uint`, AF, CF

#### `ADC_SBB_MACRO(a)`
**What it does:** Implements ADC (add with carry) and SBB (subtract with borrow). Adds/subtracts carry flag to the operation, then sets CF and AF/OF.

**Used by:** Opcode 9, cases 2 (ADC) and 3 (SBB).

**Reads:** `regs8[FLAG_CF]`, `op_result`, `op_dest`
**Writes:** CF, AF, OF via `set_AF_OF_arith()`

### Stack Macros

#### `R_M_PUSH(a)`
**What it does:** Pushes a 16-bit value onto the stack. Decrements SP by 2, then writes `a` to SS:SP. Forces `i_w=1`.

**Used by:** PUSH instructions, CALL, INT, PUSHF.

**Reads:** `regs16[REG_SS]`, `regs16[REG_SP]`
**Writes:** `regs16[REG_SP]` (decremented), `mem[SS:SP]`, `i_w`

#### `R_M_POP(a)`
**What it does:** Pops a 16-bit value from the stack into `a`. Reads from SS:SP-2, then increments SP by 2. Forces `i_w=1`.

**Used by:** POP instructions, RET, IRET, POPF.

**Reads:** `regs16[REG_SS]`, `regs16[REG_SP]`, `mem[SS:SP]`
**Writes:** `regs16[REG_SP]` (incremented), destination `a`, `i_w`

### String Operation Macros

#### `INDEX_INC(reg_id)`
**What it does:** Increments or decrements SI or DI by 1 (byte) or 2 (word), depending on the direction flag (DF).

**Used by:** String instructions MOVS, STOS, LODS (opcode 17), CMPS, SCAS (opcode 18).

**Reads:** `regs8[FLAG_DF]`, `i_w`, `regs16[reg_id]`
**Writes:** `regs16[reg_id]`

### Utility Macros

#### `TOP_BIT`
**What it does:** Returns 8 for byte operations, 16 for word operations: `8*(i_w + 1)`.

**Used by:** Flag computations, shift/rotate operations.

#### `SIGN_OF(a)`
**What it does:** Extracts sign bit of an 8-bit or 16-bit value based on `i_w`.

**Used by:** Flag computations (SF, OF), CBW, CWD, shift/rotate.

#### `CAST(a)`
**What it does:** Reinterpretation cast: `*(a*)&`. Allows treating a memory location as a different type without copying.

**Used by:** Throughout — `R_M_OP`, `MUL_MACRO`, `DIV_MACRO`, memory reads, and more.

### Opcode Structure Macros

#### `OPCODE`
**What it does:** Expands to `;break; case` — ends previous case and starts new one in the opcode switch.

#### `OPCODE_CHAIN`
**What it does:** Expands to `; case` — falls through from previous case (no break).

### Keyboard/Input Macros

#### `KEYBOARD_DRIVER`
**What it does:** (Unix) Non-blocking read of 1 byte from stdin into `mem[0x4A6]`, then fires INT 7 (keyboard interrupt). Sets `int8_asap` if ESC (0x1B) is pressed.

**Reads:** stdin (fd 0)
**Writes:** `mem[0x4A6]`, `int8_asap`

#### `SDL_KEYBOARD_DRIVER`
**What it does:** In NO_GRAPHICS mode, same as `KEYBOARD_DRIVER`. With SDL, polls SDL events for key up/down, encodes modifier state into a 16-bit value at `mem[0x4A6]`, fires INT 7.

---

## C. External Dependency Inventory

### File I/O (POSIX)

| Call | Location | Purpose | Data flow | Platform abstraction |
|------|----------|---------|-----------|---------------------|
| `open(*argv, 32898)` | Line 284 | Open BIOS, FD, and HD image files. Flag 32898 = `O_RDWR \| O_BINARY` (0x8002) | argv → disk[0..2] file descriptors | Replace with platform file/buffer abstraction |
| `read(disk[2], regs8 + 0x100, 0xFF00)` | Line 290 | Load BIOS image into memory at F000:0100 | BIOS file → mem[0xF0100] | Part of init — load from ArrayBuffer in browser |
| `read(0, mem + 0x4A6, 1)` | Line 147 (macro) | Non-blocking keyboard read from stdin | stdin → mem[0x4A6] | Replace with keyboard event queue |
| `read(disk[..], mem + ES:BX, AX)` | Line 677 | BIOS disk read (INT 0F 02) | disk file → emulated memory | Replace with platform disk abstraction |
| `write(1, regs8, 1)` | Line 668 | PUTCHAR_AL (custom opcode 0F 00): write AL to stdout | regs8[0] → stdout | Replace with console output callback |
| `write(disk[..], mem + ES:BX, AX)` | Line 677 | BIOS disk write (INT 0F 03) | emulated memory → disk file | Replace with platform disk abstraction |
| `lseek(disk[..], BP<<9, 0)` | Line 676 | Seek to sector for disk read/write | Sector number (BP) → file position | Part of disk abstraction |
| `lseek(*disk, 0, 2)` | Line 287 | Get HD image size (seek to end) | Returns file size | Part of init — pass size explicitly |

### Time

| Call | Location | Purpose | Platform abstraction |
|------|----------|---------|---------------------|
| `time(&clock_buf)` | Line 670 | Get current wall-clock time for RTC (INT 0F 01) | Replace with host time callback |
| `ftime(&ms_clock)` | Line 671 | Get millisecond-resolution time for RTC | Replace with host time callback |
| `localtime(&clock_buf)` | Line 672 | Convert time_t to struct tm, copy to guest memory | Part of RTC callback |
| `memcpy(...)` | Line 672 | Copy struct tm to emulated memory at ES:BX | Standard — keep or inline |

### SDL (guarded by `#ifndef NO_GRAPHICS`)

| Call | Purpose | Platform abstraction |
|------|---------|---------------------|
| `SDL_Init(SDL_INIT_AUDIO)` | Initialize SDL audio subsystem | Replace with Web Audio / host audio callback |
| `SDL_Init(SDL_INIT_VIDEO)` | Initialize SDL video on first graphics mode switch | Replace with canvas/framebuffer callback |
| `SDL_OpenAudio(&sdl_audio, 0)` | Open audio device | Host audio abstraction |
| `SDL_PauseAudio(...)` | Pause/resume audio | Host audio abstraction |
| `SDL_SetVideoMode(X, Y, 8, 0)` | Create display surface | Host video abstraction |
| `SDL_Flip(sdl_screen)` | Present frame to display | Host video abstraction (frame callback) |
| `SDL_PollEvent(&sdl_event)` | Poll for keyboard events | Host keyboard event queue |
| `SDL_PumpEvents()` | Process event queue | Not needed with event abstraction |
| `SDL_EnableUNICODE(1)` | Enable unicode key translation | Not needed |
| `SDL_EnableKeyRepeat(500, 30)` | Enable key repeat | Host handles this |
| `SDL_QuitSubSystem(SDL_INIT_VIDEO)` | Close video when returning to text mode | Host video abstraction |
| `SDL_Quit()` | Cleanup on exit | Host cleanup |
| `audio_callback(...)` | SDL audio callback — generates square wave from PIT timer values | Replace with host audio callback |

### Other libc

| Call | Purpose | Notes |
|------|---------|-------|
| `memcpy()` | Copy struct tm to guest memory | Standard, keep |
| `getch()` / `kbhit()` | Windows keyboard input (in `#ifdef _WIN32`) | Not relevant for Linux/browser target |

---

## D. Main Loop Analysis

### Initialization (lines 258–296)

1. **SDL init** (if enabled): Initialize audio subsystem
2. **Register setup**: `regs8` and `regs16` point to `mem + 0xF0000` (memory-mapped registers). CS = 0xF000.
3. **Boot device**: Set DL = 0x00 (FD) or 0x80 (HD) based on `@` prefix on HD argument
4. **Open disk images**: Loop opens BIOS (disk[2]), FD (disk[1]), HD (disk[0]) via positional argv
5. **HD size**: Store HD size in sectors in CX:AX
6. **Load BIOS**: Read BIOS binary into F000:0100, set IP = 0x100
7. **Load decode tables**: Copy 20 × 256-byte tables from BIOS image into `bios_table_lookup[20][256]`. Table pointers are stored in the BIOS at offsets pointed to by `regs16[0x81 + i]` (word values at F000:0102, F000:0104, ..., F000:0128).

### Main Execution Loop (lines 298–754)

The loop condition is: `opcode_stream = mem + 16*CS + IP; opcode_stream != mem` — i.e., terminates when CS:IP == 0:0.

#### Phase 1: Instruction Fetch and Decode (lines 300–333)

1. **Set opcode**: `set_opcode(*opcode_stream)` translates raw opcode via `TABLE_XLAT_OPCODE` to get `xlat_opcode_id` (the switch case number). Also loads `extra` (sub-function), `i_mod_size` (whether ModRM exists), and `set_flags_type`.

2. **Extract fields**: `i_w` (word bit) and `i_d` (direction bit) extracted from low bits of raw opcode. `i_reg4bit` = low 3 bits of opcode (used for register-encoded opcodes).

3. **Extract data bytes**: `i_data0`, `i_data1`, `i_data2` are sign-extended reads of bytes at opcode+1, +2, +3.

4. **Decrement override counters**: `seg_override_en` and `rep_override_en` count down each instruction. They're set to 2 by prefix instructions, so they survive for the next instruction then expire.

5. **ModRM decode**: If `i_mod_size > 0`, extract `i_mod`, `i_rm`, `i_reg` from the ModRM byte, adjust `i_data1`/`i_data2` for displacement size, then call `DECODE_RM_REG` to compute effective addresses.

#### Phase 2: Opcode Dispatch (lines 336–680)

A `switch (xlat_opcode_id)` with ~49 cases (0–48) handles all instructions. The `OPCODE` and `OPCODE_CHAIN` macros generate the case labels. Key groupings:

| xlat_opcode_id | Instructions |
|----------------|-------------|
| 0 | Conditional jumps (Jcc) |
| 1 | MOV reg, imm |
| 2 | INC/DEC regs16 |
| 3 | PUSH regs16 |
| 4 | POP regs16 |
| 5 | INC/DEC/JMP/CALL/PUSH r/m (FF group) |
| 6 | TEST/NOT/NEG/MUL/IMUL/DIV/IDIV (F6/F7 group) |
| 7 | ALU AL/AX, imm (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) |
| 8 | ALU r/m, imm (80-83 group) |
| 9 | ALU/MOV reg, r/m (main ALU + MOV) — nested switch on `extra` |
| 10 | MOV sreg / POP r/m / LEA |
| 11 | MOV AL/AX, [moffs] |
| 12 | Shifts and rotates (ROL/ROR/RCL/RCR/SHL/SHR/SAR) |
| 13 | LOOPxx / JCXZ |
| 14 | JMP / CALL near/far |
| 15 | TEST reg, r/m |
| 16–24 | XCHG AX,reg / NOP / XCHG reg,r/m |
| 17 | MOVS / STOS / LODS (string ops, no compare) |
| 18 | CMPS / SCAS (string compare ops) |
| 19 | RET / RETF / IRET |
| 20 | MOV r/m, imm |
| 21 | IN AL/AX, port |
| 22 | OUT port, AL/AX |
| 23 | REP / REPNZ prefix |
| 25 | PUSH reg (segment registers) |
| 26 | POP reg (segment registers) |
| 27 | Segment override prefixes |
| 28 | DAA / DAS |
| 29 | AAA / AAS |
| 30 | CBW |
| 31 | CWD |
| 32 | CALL far imm |
| 33 | PUSHF |
| 34 | POPF |
| 35 | SAHF |
| 36 | LAHF |
| 37 | LES / LDS |
| 38 | INT 3 |
| 39 | INT imm8 |
| 40 | INTO |
| 41 | AAM |
| 42 | AAD |
| 43 | SALC (undocumented) |
| 44 | XLAT |
| 45 | CMC |
| 46 | CLC/STC/CLI/STI/CLD/STD |
| 47 | TEST AL/AX, imm |
| 48 | Emulator-specific 0F xx opcodes (PUTCHAR, RTC, DISK_READ, DISK_WRITE) |

#### Phase 3: IP Advancement (line 684)

```c
reg_ip += (i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6))*i_mod_size
        + bios_table_lookup[TABLE_BASE_INST_SIZE][raw_opcode_id]
        + bios_table_lookup[TABLE_I_W_SIZE][raw_opcode_id]*(i_w + 1);
```

This computes instruction length from:
- **ModRM displacement**: `i_mod*(i_mod!=3)` bytes for mod 1/2, plus 2 extra bytes for the `[disp16]` special case (mod=0, rm=6)
- **Base instruction size**: From `TABLE_BASE_INST_SIZE` lookup
- **Immediate data size**: From `TABLE_I_W_SIZE` lookup, multiplied by operand size (1 or 2)

#### Phase 4: Flag Updates (lines 687–698)

If `set_flags_type` has `FLAGS_UPDATE_SZP` set:
- **SF** = sign bit of `op_result`
- **ZF** = `!op_result`
- **PF** = parity lookup from `TABLE_PARITY_FLAG`

If additionally `FLAGS_UPDATE_AO_ARITH`: set AF and OF via `set_AF_OF_arith()`.
If `FLAGS_UPDATE_OC_LOGIC`: clear CF and OF (logic ops).

#### Phase 5: Timer and Keyboard Polling (lines 700–753)

1. **Timer polling** (line 701): Every `KEYBOARD_TIMER_UPDATE_DELAY` (20000) instructions, set `int8_asap = 1`.

2. **Graphics update** (lines 704–741, SDL only): Every `GRAPHICS_UPDATE_DELAY` (360000) instructions:
   - If graphics mode active (`io_ports[0x3B8] & 2`): render from video RAM to SDL surface
   - If returning to text mode: close SDL window
   - Pump SDL events

3. **Trap flag** (lines 744–748): If `trap_flag` was set by previous instruction, fire INT 1.

4. **Timer/keyboard service** (lines 752–753): If `int8_asap` is set AND interrupts enabled AND no prefix overrides active:
   - Fire INT 0xA (timer tick — BIOS redirects INT 8 to INT 0xA)
   - Clear `int8_asap`
   - Run `SDL_KEYBOARD_DRIVER` to poll for keystrokes

---

## E. Instruction Encoding Tables

The BIOS binary contains 20 lookup tables of 256 bytes each, loaded at startup into `bios_table_lookup[0..19][0..255]`. These tables are the heart of 8086tiny's compact design — they replace what would otherwise be hundreds of lines of decode logic.

### Table loading mechanism

During init (lines 293–295):
```c
for (int i = 0; i < 20; i++)
    for (int j = 0; j < 256; j++)
        bios_table_lookup[i][j] = regs8[regs16[0x81 + i] + j];
```

The BIOS image (loaded at F000:0100) contains an array of 20 word-sized pointers starting at offset 0x81 relative to the register base (i.e., at F000:0102 through F000:0128). Each pointer gives the offset within the BIOS where that table's 256-byte data begins.

### Table descriptions

| Index | Constant | Purpose |
|-------|----------|---------|
| 0–3 | (addressing mode tables) | ModRM effective address computation. Tables 0–3 are indexed by `i_rm` and `i_mod` (via `scratch2_uint = 4*!i_mod`). They provide: [0] base register index, [1] index register for address calc, [2] multiplier for i_data1 displacement, [3] default segment register. |
| 4–7 | (same, for mod≠0) | Same structure as 0–3 but for `i_mod != 0` (non-zero mod field). |
| 8 | `TABLE_XLAT_OPCODE` | Maps raw opcode byte (0x00–0xFF) to translated opcode ID (the `xlat_opcode_id` used in the main switch). This is the primary dispatch table. Many opcodes map to the same xlat ID (e.g., all ALU reg,r/m variants map to 9). |
| 9 | `TABLE_XLAT_SUBFUNCTION` | Maps raw opcode to `extra` — a sub-function index. For opcode groups (like 0x80–0x83 ALU immediate), `extra` distinguishes ADD/OR/ADC/SBB/AND/SUB/XOR/CMP. For segment overrides, `extra` is the segment register index. |
| 10 | `TABLE_STD_FLAGS` | Maps raw opcode to a bitfield indicating which flag groups to update: bit 0 = SZP, bit 1 = AO (arithmetic), bit 2 = OC (logic: clear OF and CF). |
| 11 | `TABLE_PARITY_FLAG` | 256-byte parity lookup: `TABLE_PARITY_FLAG[x]` = 1 if `x` has even parity, 0 otherwise. Indexed by `(unsigned char)op_result`. |
| 12 | `TABLE_BASE_INST_SIZE` | Base instruction size in bytes for each raw opcode (excluding ModRM displacement and immediate data). Used in IP advancement. |
| 13 | `TABLE_I_W_SIZE` | Additional bytes for immediate data based on operand size. Value is multiplied by `(i_w + 1)` during IP advancement. |
| 14 | `TABLE_I_MOD_SIZE` | Whether the opcode uses a ModRM byte (1) or not (0). Controls whether ModRM decode runs. |
| 15 | `TABLE_COND_JUMP_DECODE_A` | Conditional jump decode: first flag to test. Indexed by condition code (0–7). |
| 16 | `TABLE_COND_JUMP_DECODE_B` | Conditional jump decode: second flag to test (OR'd with A). |
| 17 | `TABLE_COND_JUMP_DECODE_C` | Conditional jump decode: third flag to test (XOR'd with D). |
| 18 | `TABLE_COND_JUMP_DECODE_D` | Conditional jump decode: fourth flag to test (XOR'd with C). Together, tables 15–18 encode: `jump if (A \|\| B \|\| (C ^ D))`. |
| 19 | `TABLE_FLAGS_BITFIELDS` | Maps flag index (0–8, for CF through OF) to bit position in the FLAGS register. Used by `make_flags()` and `set_flags()` to pack/unpack the FLAGS word. |

### How the xlat system works

The real 8086 has ~256 base opcodes, but many are variants of the same operation (e.g., ADD has ~8 encodings). The xlat table collapses these into ~49 "translated" opcodes. The `extra` field distinguishes sub-operations within a group. For example:

- Raw opcodes 0x00–0x05 (ADD variants) → xlat 9, extra 0
- Raw opcodes 0x08–0x0D (OR variants) → xlat 9, extra 1
- Raw opcodes 0x80–0x83 (ALU r/m, imm) → xlat 8, extra = reg field from ModRM

This lets a single switch case handle all ALU operations, with `extra` selecting the specific operation.

### Addressing mode decode (tables 0–7)

For `i_mod == 0` (no displacement, or 16-bit displacement for rm=6), tables 0–3 are used.
For `i_mod != 0` (8-bit or 16-bit displacement), tables 4–7 are used.

The effective address formula is:
```
EA = SEGREG(table[3][i_rm], table[0][i_rm], regs16[table[1][i_rm]] + table[2][i_rm] * i_data1)
```

Where:
- `table[3][i_rm]` = segment register (DS, SS, etc.)
- `table[0][i_rm]` = base register (BX, BP, etc.)
- `table[1][i_rm]` = index register (SI, DI, or zero)
- `table[2][i_rm]` = displacement multiplier (0 or 1)

UNCLEAR: The exact contents of tables 0–7 are embedded in the BIOS binary and not directly visible in the C source. The above is inferred from the `DECODE_RM_REG` macro and 8086 addressing mode rules. To fully verify, one would need to disassemble the BIOS or dump these tables at runtime.

---

## Notes

### Memory layout

The `mem` array (0x10FFF0 bytes) maps the full 8086 address space:
- `0x00000–0x9FFFF`: Conventional RAM (640 KB)
- `0xA0000–0xBFFFF`: Video RAM (EGA/VGA area, includes 0xB0000 Hercules and 0xB8000 CGA)
- `0xC0000–0xEFFFF`: ROM area (unused in emulator)
- `0xF0000–0xFFFFF`: BIOS area. Registers are memory-mapped at 0xF0000. BIOS code loaded at 0xF0100.

### Register memory mapping

Registers occupy `mem[0xF0000]` onwards:
- 16-bit regs: AX(+0), CX(+2), DX(+4), BX(+6), SP(+8), BP(+10), SI(+12), DI(+14)
- Segment regs: ES(+16), CS(+18), SS(+20), DS(+22)
- Special: REG_ZERO(+24) = always 0, REG_SCRATCH(+26)
- Flags are stored as individual bytes after the registers

### BIOS data area locations used by emulator

| Address | Purpose |
|---------|---------|
| `0x4A6` | Keyboard scancode buffer (2 bytes with SDL) |
| `0x4AA` | PIT timer counter value (for audio) |
| `0x4AC` | CGA mode flag (0 = Hercules, nonzero = CGA) |
| `0x4AD` | Video RAM start offset (2 bytes) |
| `0x469–0x46A` | PIT channel rate (alternating high/low byte programming) |
| `0x49D` | Cursor column |
| `0x49E` | Cursor row |

### Undocumented/unusual features

- **SALC** (opcode 0xD6): Undocumented 8086 instruction "Set AL from Carry". Sets AL = 0xFF if CF=1, AL = 0x00 if CF=0.
- **Custom 0x0F opcodes**: The emulator repurposes the 8086's unused 0x0F prefix for emulator-specific functions: PUTCHAR (0F 00), GET_RTC (0F 01), DISK_READ (0F 02), DISK_WRITE (0F 03). These are called by the custom BIOS.
- **Flag storage**: Individual flags are stored as separate bytes in the register area, not as a packed FLAGS register. `make_flags()` and `set_flags()` convert between representations.
- **Open flags 32898**: `open(*argv, 32898)` — decimal 32898 = 0x8082 = `O_RDWR | O_BINARY` (O_BINARY is Windows-specific, silently ignored on POSIX but the numeric constant may differ).

UNCLEAR: Whether `32898` is portable across all POSIX systems. On Linux, `O_RDWR` = 2 and there is no `O_BINARY`, so the extra bits (0x8080) map to `O_LARGEFILE` on some architectures. This works in practice but is not strictly portable.
