## Task: EMU-10

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-09: All core structs, decoder, arithmetic, logic, shift/rotate, data transfer, string, control flow opcodes. All tests passing.

### Your task

**Goal:** Implement the remaining opcodes: flag manipulation (CLC/STC/CMC/CLI/STI/CLD/STD), I/O port instructions (IN/OUT), and miscellaneous instructions (NOP, HLT, SALC, segment override and REP prefix handlers). Also implement the emulator-specific 0x0F BIOS call opcodes (PUTCHAR, GET_RTC, DISK_READ, DISK_WRITE).

This is the last opcode task before EMU-11 wires everything into the run loop.

### The operations

**Flag manipulation — all single-byte, no operands**

| Opcode | Mnemonic | Action |
|--------|----------|--------|
| F8 | CLC | CF = 0 |
| F9 | STC | CF = 1 |
| F5 | CMC | CF = !CF (complement) |
| FA | CLI | IF = 0 (disable interrupts) |
| FB | STI | IF = 1 (enable interrupts) |
| FC | CLD | DF = 0 (string ops go forward) |
| FD | STD | DF = 1 (string ops go backward) |

No other flags affected by any of these.

**I/O Port Instructions**

These read/write the `io_ports[]` array and are how the guest talks to hardware (timer, keyboard controller, disk, NIC, etc.)

- **IN AL, imm8 (E4)** — `AL = io_ports[imm8]`
- **IN AX, imm8 (E5)** — `AX = io_ports[imm8] | (io_ports[imm8+1] << 8)`
- **IN AL, DX (EC)** — `AL = io_ports[DX]`
- **IN AX, DX (ED)** — `AX = io_ports[DX] | (io_ports[DX+1] << 8)`
- **OUT imm8, AL (E6)** — `io_ports[imm8] = AL`
- **OUT imm8, AX (E7)** — `io_ports[imm8] = AL, io_ports[imm8+1] = AH`
- **OUT DX, AL (EE)** — `io_ports[DX] = AL`
- **OUT DX, AX (EF)** — `io_ports[DX] = AL, io_ports[DX+1] = AH`

No flags affected.

**Important:** Some OUT instructions trigger side effects. In the original 8086tiny, writes to specific ports control the PIT timer, speaker, and CGA mode. For now, just read/write `io_ports[]`. The run loop (EMU-11) and device emulation (later) will watch for specific port writes. But create a hook point:

```c
// Called after every OUT instruction. The run loop or device layer
// can use this to detect writes to special ports.
// For now, this is a no-op stub.
static inline void io_port_write_hook(Emu86State *s, uint16_t port, uint8_t value);
```

**Miscellaneous**

- **NOP (90)** — Do nothing. (This is actually XCHG AX,AX which was already in EMU-07, but ensure it's handled.)

- **HLT (F4)** — Set `state->halted = 1`. The run loop will yield when it sees this. Execution resumes when a hardware interrupt arrives.

- **SALC (D6)** — Undocumented 8086 instruction: `AL = CF ? 0xFF : 0x00`. No flags affected.

**Prefix handlers**

These aren't really "executed" — they modify the decode state for the NEXT instruction. The decoder already handles `seg_override_en`/`rep_override_en` countdown. But the run loop needs to recognise prefix opcodes and NOT advance IP by the normal amount for the following instruction. The prefix opcodes:

- **Segment overrides: 26 (ES:), 2E (CS:), 36 (SS:), 3E (DS:)** — Set `seg_override_en = 2`, `seg_override = segment_index`
- **REP/REPZ (F3), REPNZ (F2)** — Set `rep_override_en = 2`, `rep_mode = 0 or 1`
- **LOCK (F0)** — ignored on 8086 (no multiprocessor). Treat as NOP.

These are already partially handled by the decoder's countdown mechanism. The exec functions just need to set the state fields:

```c
static inline void exec_segment_override(Emu86State *s, uint8_t sreg);
static inline void exec_rep(Emu86State *s, uint8_t mode);  // mode: 0=REPNZ, 1=REPZ
```

**Emulator-specific 0x0F opcodes (BIOS calls)**

The original 8086tiny repurposes the 0x0F prefix for custom BIOS-emulator communication. These are called by the BIOS code (not by user programs). They bridge the emulator to the host platform:

- **0F 00 — PUTCHAR_AL** — Write AL to console output
- **0F 01 — GET_RTC** — Read real-time clock into guest memory at ES:BX
- **0F 02 — DISK_READ** — Read sectors from disk
- **0F 03 — DISK_WRITE** — Write sectors to disk

These call into the `Emu86Platform` interface:

```c
// These functions use the platform callbacks/ring buffers.
// They take the platform struct as a parameter.
static inline void exec_bios_putchar(Emu86State *s, Emu86Platform *p);
static inline void exec_bios_get_rtc(Emu86State *s, Emu86Platform *p);
static inline int exec_bios_disk_read(Emu86State *s, Emu86Platform *p);
static inline int exec_bios_disk_write(Emu86State *s, Emu86Platform *p);
```

**PUTCHAR_AL:** Write `state->regs[REG_AX] & 0xFF` (AL) to `platform->console_out` ring buffer.

**GET_RTC:** Call `platform->get_time_us()`, convert to a struct tm-like format, and write to guest memory at ES:BX. Match the original's format: the BIOS expects `memcpy(mem + ES:BX, &tm_struct, sizeof(struct tm))`. For now, implement a simplified version that writes the time fields the BIOS actually uses.

**DISK_READ:** Seek to sector `BP << 9` (BP * 512) on drive `DL`, read `AX` bytes into `mem[ES:BX]` via `platform->disk_read()`. Match the original: drive 0=HD, 1=FD. Set AL = 0 on success, non-zero on error.

**DISK_WRITE:** Same as DISK_READ but writes from `mem[ES:BX]` to disk via `platform->disk_write()`.

Refer to the original 8086tiny.c opcode 48 (line ~660–680 in the analysis) for the exact register conventions.

### Files to create

**`src/emulator/opcodes/flags_io.h`** (or `misc.h` — your choice of naming)

```c
// Flag manipulation
static inline void exec_clc(Emu86State *s);
static inline void exec_stc(Emu86State *s);
static inline void exec_cmc(Emu86State *s);
static inline void exec_cli(Emu86State *s);
static inline void exec_sti(Emu86State *s);
static inline void exec_cld(Emu86State *s);
static inline void exec_std(Emu86State *s);

// I/O ports
static inline void exec_in_al_imm(Emu86State *s, uint8_t port);
static inline void exec_in_ax_imm(Emu86State *s, uint8_t port);
static inline void exec_in_al_dx(Emu86State *s);
static inline void exec_in_ax_dx(Emu86State *s);
static inline void exec_out_imm_al(Emu86State *s, uint8_t port);
static inline void exec_out_imm_ax(Emu86State *s, uint8_t port);
static inline void exec_out_dx_al(Emu86State *s);
static inline void exec_out_dx_ax(Emu86State *s);
static inline void io_port_write_hook(Emu86State *s, uint16_t port, uint8_t value);

// Misc
static inline void exec_hlt(Emu86State *s);
static inline void exec_salc(Emu86State *s);

// Prefix handlers
static inline void exec_segment_override(Emu86State *s, uint8_t sreg);
static inline void exec_rep_prefix(Emu86State *s, uint8_t mode);

// Emulator BIOS calls (0x0F xx)
static inline void exec_bios_putchar(Emu86State *s, Emu86Platform *p);
static inline void exec_bios_get_rtc(Emu86State *s, Emu86Platform *p);
static inline int exec_bios_disk_read(Emu86State *s, Emu86Platform *p);
static inline int exec_bios_disk_write(Emu86State *s, Emu86Platform *p);
```

**`test/unit/test_flags_io.c`**

```
=== Flag manipulation ===

TEST: clc
  - CF=1, CLC → CF=0, all other flags unchanged

TEST: stc
  - CF=0, STC → CF=1, all other flags unchanged

TEST: cmc_set_to_clear
  - CF=1, CMC → CF=0

TEST: cmc_clear_to_set
  - CF=0, CMC → CF=1

TEST: cli
  - IF=1, CLI → IF=0

TEST: sti
  - IF=0, STI → IF=1

TEST: cld
  - DF=1, CLD → DF=0

TEST: std
  - DF=0, STD → DF=1

TEST: flag_ops_preserve_other_flags
  - Set all flags to known state
  - Execute CLC → only CF changed
  - Execute STI → only IF changed
  - etc.

=== I/O ports ===

TEST: in_al_imm
  - io_ports[0x60] = 0x42
  - IN AL, 0x60 → AL = 0x42

TEST: in_ax_imm
  - io_ports[0x60] = 0x34, io_ports[0x61] = 0x12
  - IN AX, 0x60 → AX = 0x1234

TEST: in_al_dx
  - DX = 0x3D4, io_ports[0x3D4] = 0xAB
  - IN AL, DX → AL = 0xAB

TEST: in_ax_dx
  - DX = 0x3D4, io_ports[0x3D4] = 0xCD, io_ports[0x3D5] = 0xAB
  - IN AX, DX → AX = 0xABCD

TEST: out_imm_al
  - AL = 0x42
  - OUT 0x60, AL → io_ports[0x60] = 0x42

TEST: out_imm_ax
  - AX = 0x1234
  - OUT 0x60, AX → io_ports[0x60] = 0x34, io_ports[0x61] = 0x12

TEST: out_dx_al
  - DX = 0x3D4, AL = 0x42
  - OUT DX, AL → io_ports[0x3D4] = 0x42

TEST: out_dx_ax
  - DX = 0x3D4, AX = 0x1234
  - OUT DX, AX → io_ports[0x3D4] = 0x34, io_ports[0x3D5] = 0x12

TEST: io_no_flags
  - Set flags, IN/OUT, assert unchanged

=== HLT ===

TEST: hlt_sets_halted
  - halted=0, HLT → halted=1

TEST: hlt_no_flags
  - Flags unchanged after HLT

=== SALC ===

TEST: salc_cf_set
  - CF=1, SALC → AL=0xFF

TEST: salc_cf_clear
  - CF=0, SALC → AL=0x00

TEST: salc_no_flags
  - Flags unchanged after SALC

=== Prefix handlers ===

TEST: segment_override_sets_state
  - exec_segment_override(s, SREG_ES)
  - Assert seg_override_en = 2, seg_override = SREG_ES

TEST: rep_prefix_sets_state
  - exec_rep_prefix(s, 1)  // REPZ
  - Assert rep_override_en = 2, rep_mode = 1

=== BIOS calls ===

TEST: bios_putchar
  - Set AL = 'A' (0x41)
  - Set up console_out ring buffer
  - exec_bios_putchar
  - Read from ring buffer → assert 0x41

TEST: bios_putchar_multiple
  - Write 'H', 'i' via BIOS putchar
  - Read from ring buffer → "Hi"

TEST: bios_disk_read
  - Set up a mock disk_read callback that returns known data
  - Set registers: BP (sector), AX (byte count), ES:BX (dest), DL (drive)
  - exec_bios_disk_read
  - Assert memory at ES:BX contains expected data

TEST: bios_disk_write
  - Write known data to memory at ES:BX
  - Set up mock disk_write callback
  - exec_bios_disk_write
  - Assert callback received correct data

TEST: bios_get_rtc
  - Set up mock get_time_us callback returning a known timestamp
  - exec_bios_get_rtc
  - Assert time fields written to ES:BX in expected format
```

### Deliverables
1. `src/emulator/opcodes/flags_io.h` (or `misc.h`) — All remaining opcode implementations
2. `test/unit/test_flags_io.c` — All tests listed above, passing
3. Updated Makefile with `test-flags-io` target
4. All previous tests still pass (`make test-unit`)

### Rules
- All functions `static inline`
- Flag manipulation instructions affect ONLY the specific flag named — nothing else
- I/O instructions do NOT affect flags
- HLT sets halted=1 but does NOT affect flags or IP
- SALC does NOT affect flags
- The BIOS call implementations (0x0F xx) must use the platform interface (ring buffers, callbacks), NOT direct OS calls
- `io_port_write_hook` is a stub for now — it will be filled in by device emulation later
- The disk read/write BIOS calls must handle the case where the platform callback returns an error (disk not present, etc.)

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-10
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu10-task.md tasks/completed/
   git add -A
   git commit -m "EMU-10: Flags, I/O, and misc opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-10: Flags, I/O, and misc opcodes (PARTIAL - see task log)"
