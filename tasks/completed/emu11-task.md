## Task: EMU-11

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-10: All core structs, decoder, and every opcode family implemented. 1016 assertions across 10 test suites, all passing.

### Your task

**Goal:** Create `emu86_run()` — the batch execution loop that wires together the decoder, all opcode implementations, interrupt handling, timer simulation, keyboard input, and the yield mechanism. This is the function that makes the emulator go.

Also create `emu86_step_single()` for debugging/testing — executes exactly one instruction.

When this task is complete, the emulator core is fully functional. EMU-12 (Linux host) will wrap it in a CLI tool that actually boots an OS.

### Design — the unity build

`src/emulator/run.c` is the **single compilation unit** for the hot path. It `#include`s all the opcode headers so the compiler sees the entire emulator as one function body and inlines everything aggressively:

```c
// run.c — unity build
#include "state.h"
#include "platform.h"
#include "tables.h"
#include "decode.h"
#include "opcodes/helpers.h"
#include "opcodes/arithmetic.h"
#include "opcodes/logic.h"
#include "opcodes/shift.h"
#include "opcodes/transfer.h"
#include "opcodes/string.h"
#include "opcodes/control.h"
#include "opcodes/flags_io.h"   // or whatever EMU-10 named it

// The run loop and dispatch logic follow...
```

### Design — emu86_run()

```c
typedef struct {
    int      reason;        // why we yielded
    uint32_t cycles_used;   // cycles consumed this batch
    uint8_t  io_type;       // for IO_NEEDED: what kind
    uint16_t io_port;       // for IO_NEEDED: which port
} Emu86YieldInfo;

#define EMU86_YIELD_BUDGET     0  // hit the cycle limit
#define EMU86_YIELD_HALTED     1  // HLT instruction
#define EMU86_YIELD_IO_NEEDED  2  // console_out buffer nearly full
#define EMU86_YIELD_BREAKPOINT 3  // debug breakpoint (future)
#define EMU86_YIELD_ERROR      4  // fatal: bad opcode, triple fault
#define EMU86_YIELD_EXIT       5  // CS:IP == 0000:0000 (emulator exit)

void emu86_run(Emu86State *state, Emu86Platform *platform,
               Emu86Tables *tables, uint32_t cycle_budget,
               Emu86YieldInfo *yield);
```

### The run loop — step by step

```c
void emu86_run(Emu86State *s, Emu86Platform *p,
               Emu86Tables *t, uint32_t budget,
               Emu86YieldInfo *yield)
{
    uint32_t cycles = 0;
    DecodeContext d;

    while (cycles < budget) {
        // --- 1. Check for emulator exit condition ---
        // Original 8086tiny exits when CS:IP == 0000:0000
        // (the QUITEMU.COM program jumps here)
        if (s->sregs[SREG_CS] == 0 && s->ip == 0) {
            yield->reason = EMU86_YIELD_EXIT;
            yield->cycles_used = cycles;
            return;
        }

        // --- 2. Handle pending hardware interrupts ---
        if (s->int_pending && (s->flags & FLAG_IF) &&
            s->seg_override_en == 0 && s->rep_override_en == 0) {
            // Don't interrupt during prefix sequences
            pc_interrupt(s, s->int_vector);
            s->int_pending = 0;
        }

        // --- 3. Check HLT state ---
        if (s->halted) {
            yield->reason = EMU86_YIELD_HALTED;
            yield->cycles_used = cycles;
            return;
        }

        // --- 4. Decode the instruction at CS:IP ---
        memset(&d, 0, sizeof(d));
        decode_instruction(s, t, &d);

        // --- 5. Decrement prefix override counters ---
        if (s->seg_override_en)
            s->seg_override_en--;
        if (s->rep_override_en)
            s->rep_override_en--;

        // --- 6. Execute the instruction ---
        // Dispatch based on d.xlat_id (the translated opcode)
        // This is a large switch statement mirroring 8086tiny's
        // xlat_opcode_id dispatch, but calling our named functions.
        execute_instruction(s, p, t, &d);

        // --- 7. Advance IP ---
        if (!d.ip_changed) {
            s->ip += d.inst_length;
        }

        // --- 8. Update flags if needed ---
        // The set_flags_type from the decode tables tells us which
        // flag groups to update. Most opcodes handle their own flags,
        // but if using the table-driven approach, apply here.
        // (Depends on how the opcode implementations were structured)

        // --- 9. Cycle accounting ---
        // Approximate: most 8086 instructions take 4-20 cycles.
        // For simplicity, count 4 cycles per instruction.
        // This is rough but sufficient for timer/polling intervals.
        cycles += 4;
        s->inst_count++;

        // --- 10. Trap flag handling ---
        if (s->trap_flag) {
            pc_interrupt(s, 1);  // INT 1 = single-step
            s->trap_flag = 0;
        }
        // Latch the current TF for next instruction
        if (s->flags & FLAG_TF)
            s->trap_flag = 1;

        // --- 11. Timer and keyboard polling ---
        // Every ~20000 instructions, service the timer
        if ((s->inst_count & 0x4FFF) == 0) {
            s->int8_asap = 1;
        }

        // Service pending timer interrupt
        if (s->int8_asap && (s->flags & FLAG_IF) &&
            s->seg_override_en == 0 && s->rep_override_en == 0) {
            // Fire timer interrupt (INT 8 redirected to INT 0xA by BIOS)
            pc_interrupt(s, 0xA);
            s->int8_asap = 0;

            // Poll keyboard: check console_in ring buffer
            uint8_t key;
            if (ringbuf_read(&p->console_in, &key) == 0) {
                // Deliver keystroke to guest
                // Write scancode to keyboard buffer location
                s->mem[0x4A6] = key;
                if (key == 0x1B) s->int8_asap = 1;  // ESC triggers faster polling
                pc_interrupt(s, 7);  // keyboard interrupt
            }
        }

        // --- 12. Check if console output needs flushing ---
        if (ringbuf_available(&p->console_out) > (p->console_out.size * 3 / 4)) {
            yield->reason = EMU86_YIELD_IO_NEEDED;
            yield->io_type = 0;  // console
            yield->cycles_used = cycles;
            return;
        }
    }

    yield->reason = EMU86_YIELD_BUDGET;
    yield->cycles_used = cycles;
}
```

### The dispatch function

The `execute_instruction()` function is a large switch on `d.xlat_id`:

```c
static inline void execute_instruction(Emu86State *s, Emu86Platform *p,
                                        Emu86Tables *t, DecodeContext *d)
{
    switch (d->xlat_id) {
    case 0:  // Conditional jumps (Jcc)
        exec_jcc(s, d);
        break;

    case 1:  // MOV reg, imm
        exec_mov_reg_imm(s, d);
        break;

    case 2:  // INC/DEC reg16
        if (d->extra == 0) exec_inc(s, d);
        else exec_dec(s, d);
        break;

    case 3:  // PUSH reg16
        exec_push_reg(s, d->reg4bit);
        break;

    case 4:  // POP reg16
        exec_pop_reg(s, d->reg4bit);
        break;

    // ... (continue for all ~49 xlat cases)
    // Refer to ORIGINAL-ANALYSIS.md Section D, Phase 2 for the full mapping

    case 48:  // Emulator-specific 0F xx
        switch (d->extra) {
        case 0: exec_bios_putchar(s, p); break;
        case 1: exec_bios_get_rtc(s, p); break;
        case 2: exec_bios_disk_read(s, p); break;
        case 3: exec_bios_disk_write(s, p); break;
        }
        break;

    default:
        // Unknown opcode — could set an error yield
        break;
    }
}
```

**CRITICAL:** The full switch must cover all 49 xlat cases from the original. Use `docs/ORIGINAL-ANALYSIS.md` Section D (Phase 2: Opcode Dispatch) as the definitive reference. Each `xlat_id` maps to one or more of the exec functions from EMU-04 through EMU-10.

Some cases use the `extra` field (from TABLE_XLAT_SUBFUNCTION) to select sub-operations. For example:
- xlat 9 (main ALU): `extra` selects ADD(0), OR(1), ADC(2), SBB(3), AND(4), SUB(5), XOR(6), CMP(7)
- xlat 6 (F6/F7 group): `extra` selects TEST(0), NOT(2), NEG(3), MUL(4), IMUL(5), DIV(6), IDIV(7)
- xlat 12 (shifts): `extra` selects ROL(0), ROR(1), RCL(2), RCR(3), SHL(4), SHR(5), SAR(7)

### The debug step function

```c
// Execute exactly one instruction. For debugging and test use only.
// NOT on the hot path — can be a regular function.
// Returns cycle count for the instruction (approximate).
int emu86_step_single(Emu86State *state, Emu86Platform *platform,
                      Emu86Tables *tables);
```

This is a simpler version of the run loop: decode one instruction, execute it, advance IP, handle trap flag. No batching, no yield, no timer polling. Used by:
- Test harnesses that need to step through code instruction by instruction
- Future debugger integration
- Snapshot-at-instruction-boundary verification

### Additional header: `src/emulator/run.h`

Public API for the emulator:

```c
#ifndef EMU86_RUN_H
#define EMU86_RUN_H

#include "state.h"
#include "platform.h"
#include "tables.h"

typedef struct {
    int      reason;
    uint32_t cycles_used;
    uint8_t  io_type;
    uint16_t io_port;
} Emu86YieldInfo;

#define EMU86_YIELD_BUDGET     0
#define EMU86_YIELD_HALTED     1
#define EMU86_YIELD_IO_NEEDED  2
#define EMU86_YIELD_BREAKPOINT 3
#define EMU86_YIELD_ERROR      4
#define EMU86_YIELD_EXIT       5

void emu86_run(Emu86State *state, Emu86Platform *platform,
               Emu86Tables *tables, uint32_t cycle_budget,
               Emu86YieldInfo *yield);

int emu86_step_single(Emu86State *state, Emu86Platform *platform,
                      Emu86Tables *tables);

#endif
```

### Files to create

1. **`src/emulator/run.h`** — Public API declarations
2. **`src/emulator/run.c`** — Unity build: includes all headers, contains `emu86_run()`, `execute_instruction()`, `emu86_step_single()`
3. **`test/unit/test_run.c`** — Run loop tests

### Test file: `test/unit/test_run.c`

These tests exercise the run loop with small programs placed in memory. They need the BIOS loaded for table initialization and the platform interface set up with ring buffers.

```
=== Setup ===

Each test needs:
- Emu86State initialized with emu86_init()
- BIOS loaded from reference/bios into mem[F000:0100]
- Emu86Tables loaded via emu86_load_tables()
- Emu86Platform with console_in/console_out ring buffers allocated
- A small Emu86YieldInfo struct

Helper:
  static void setup_test(Emu86State *s, Emu86Tables *t, Emu86Platform *p, ...);

=== Basic execution ===

TEST: run_single_nop
  - Place NOP (0x90) at CS:IP followed by HLT (0xF4)
  - emu86_run with budget=100
  - Assert yield reason = HALTED
  - Assert IP advanced past the NOP to the HLT
  - Assert cycles_used > 0

TEST: run_mov_and_halt
  - Place: MOV AX, 0x1234 (B8 34 12) then HLT (F4)
  - Run
  - Assert AX = 0x1234
  - Assert yield = HALTED

TEST: run_add_two_registers
  - Place: MOV AX, 5 / MOV BX, 3 / ADD AX, BX / HLT
  - Run
  - Assert AX = 8

TEST: run_budget_exhaustion
  - Place an infinite loop: JMP short -2 (EB FE)
  - Run with budget=100
  - Assert yield reason = BUDGET
  - Assert cycles_used >= 100

TEST: run_exit_condition
  - Place: JMP FAR 0000:0000 (EA 00 00 00 00)
  - Run
  - Assert yield reason = EXIT

=== Step single ===

TEST: step_single_executes_one
  - Place: MOV AX, 0x1234 / MOV BX, 0x5678 / HLT
  - step_single → AX = 0x1234, BX unchanged
  - step_single → BX = 0x5678
  - step_single → halted = 1

TEST: step_single_returns_cycles
  - step_single returns > 0

=== Arithmetic in run loop ===

TEST: run_subtract_and_compare
  - Place: MOV AX, 10 / MOV BX, 3 / SUB AX, BX / HLT
  - Run → AX = 7, CF = 0

TEST: run_compare_and_jump
  - Place: MOV AX, 5 / CMP AX, 5 / JZ target / HLT / target: MOV BX, 1 / HLT
  - Run → BX = 1 (the jump was taken)

TEST: run_compare_and_no_jump
  - Place: MOV AX, 5 / CMP AX, 3 / JZ target / MOV BX, 2 / HLT / target: MOV BX, 1 / HLT
  - Run → BX = 2 (the jump was NOT taken)

=== Loop ===

TEST: run_loop_counter
  - Place: MOV CX, 5 / MOV AX, 0 / loop_start: INC AX / LOOP loop_start / HLT
  - Run → AX = 5, CX = 0

=== Stack operations in run loop ===

TEST: run_push_pop
  - Place: MOV AX, 0xBEEF / PUSH AX / MOV AX, 0 / POP AX / HLT
  - Run → AX = 0xBEEF

TEST: run_call_ret
  - Place: CALL near subroutine / HLT / subroutine: MOV AX, 0x42 / RET
  - Run → AX = 0x42, IP at the HLT

=== Interrupt handling ===

TEST: run_software_int
  - Set up IVT entry for INT 0x20 pointing to a handler that sets BX=0xFF and does IRET
  - Place: MOV BX, 0 / INT 0x20 / HLT
  - Run → BX = 0xFF (handler ran and returned)

TEST: run_hardware_interrupt
  - Set up IVT entry for a vector
  - Set int_pending=1, int_vector=vector, IF=1
  - Place NOPs followed by HLT
  - Run → the interrupt handler should have fired

TEST: run_interrupt_when_if_clear
  - Set int_pending=1, IF=0
  - Run some NOPs
  - Assert interrupt was NOT serviced (deferred)

=== Keyboard input ===

TEST: run_keyboard_delivery
  - Write a scancode to console_in ring buffer
  - Run for enough cycles to trigger keyboard polling
  - Assert mem[0x4A6] contains the scancode (or keyboard interrupt was fired)

=== Console output ===

TEST: run_putchar_via_bios
  - Set up BIOS with the 0F 00 PUTCHAR opcode handled
  - The BIOS INT 10h handler uses 0F 00 internally
  - This test may be complex — at minimum test that exec_bios_putchar works within the run loop context

TEST: run_console_out_yield
  - Fill console_out ring buffer to near capacity
  - Execute a PUTCHAR instruction
  - Assert yield reason = IO_NEEDED

=== Timer ===

TEST: run_timer_interrupt_fires
  - Run for enough cycles (> 20000 equivalent instructions)
  - Assert that the timer interrupt mechanism triggered (int8_asap was set and serviced)

=== Prefix handling ===

TEST: run_segment_override
  - Set ES to a different segment with different data than DS
  - Place: ES: MOV AX, [addr] (segment override prefix + MOV)
  - Run → AX should contain data from ES:addr, not DS:addr

TEST: run_rep_movsb
  - Set up source and dest regions, CX=10
  - Place: REP MOVSB / HLT
  - Run → 10 bytes copied, CX=0

=== Multi-instruction programs ===

TEST: run_fibonacci
  - Implement a small program that computes fibonacci(10):
    MOV CX, 10 / MOV AX, 0 / MOV BX, 1 / loop: MOV DX, AX / ADD AX, BX / MOV BX, DX / LOOP loop / HLT
  - Run → AX = 55 (fib(10))

TEST: run_memory_fill
  - Use REP STOSB to fill a memory region
  - Place: MOV AX, 0xFF / MOV CX, 256 / MOV DI, 0x1000 / REP STOSB / HLT
  - Run → 256 bytes at ES:0x1000 filled with 0xFF
```

### How to assemble test programs

Create a helper that places instruction bytes at CS:IP:

```c
// Helper: place bytes at the current CS:IP location
static uint16_t emit(Emu86State *s, uint16_t offset, const uint8_t *bytes, int count) {
    uint32_t addr = segoff_to_linear(s->sregs[SREG_CS], offset);
    memcpy(&s->mem[addr], bytes, count);
    return offset + count;
}

// Or even simpler, use an emit pointer:
static uint8_t *code_at(Emu86State *s, uint16_t offset) {
    return &s->mem[segoff_to_linear(s->sregs[SREG_CS], offset)];
}
```

You can then write test programs as byte arrays:
```c
uint8_t prog[] = {
    0xB8, 0x34, 0x12,  // MOV AX, 0x1234
    0xBB, 0x78, 0x56,  // MOV BX, 0x5678
    0x01, 0xD8,         // ADD AX, BX
    0xF4,               // HLT
};
memcpy(code_at(s, 0x0100), prog, sizeof(prog));
```

### Makefile updates

```makefile
# The run.c unity build
build:
	$(CC) $(CFLAGS) -c src/emulator/run.c -o build/run.o -Isrc/emulator
	$(CC) $(CFLAGS) -c src/emulator/snapshot.c -o build/snapshot.o -Isrc/emulator

# Run loop tests
test-run: build
	$(CC) $(CFLAGS) -o test/unit/test_run test/unit/test_run.c src/emulator/run.c src/emulator/snapshot.c -Isrc/emulator
	./test/unit/test_run

# Build ALL of emu86 as a static library (for later linking with hosts)
lib:
	$(CC) $(CFLAGS) -c src/emulator/run.c -o build/run.o -Isrc/emulator
	$(CC) $(CFLAGS) -c src/emulator/snapshot.c -o build/snapshot.o -Isrc/emulator
	ar rcs build/libemu86.a build/run.o build/snapshot.o
```

Note: `test-run` compiles `run.c` (which includes all opcode headers via the unity build) together with the test file. The test file should NOT include the opcode headers directly — it includes `run.h` and calls `emu86_run()` / `emu86_step_single()`.

### Deliverables
1. `src/emulator/run.h` — Public API (emu86_run, emu86_step_single, yield types)
2. `src/emulator/run.c` — Unity build with complete opcode dispatch switch
3. `test/unit/test_run.c` — All tests listed above, passing
4. Updated Makefile with `build`, `test-run`, `lib` targets
5. All previous tests still pass (`make test-unit`)

### Rules
- `run.c` is the ONLY .c file that includes the opcode headers. Everything else uses `run.h`.
- The opcode dispatch switch must cover ALL 49 xlat cases from ORIGINAL-ANALYSIS.md Section D
- Any xlat case not yet covered should print a warning and continue (not crash)
- The prefix countdown (seg_override_en, rep_override_en) must decrement BEFORE execution, not after
- Hardware interrupts must NOT fire during prefix sequences (check seg_override_en == 0 && rep_override_en == 0)
- The exit condition (CS:IP == 0000:0000) must be checked BEFORE attempting to decode
- Timer polling interval (~20000 instructions) should use a bitmask for speed, not modulo
- Console output buffer fill check should use 75% threshold for yielding
- `emu86_step_single` must handle trap flag correctly (single-step debugging depends on it)
- Cycle counting is approximate (4 per instruction is fine for v1) — it's used for timer intervals, not accuracy

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-11
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues encountered, design decisions, deviations from spec}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu11-task.md tasks/completed/
   git add -A
   git commit -m "EMU-11: Run loop and opcode dispatch"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-11: Run loop and opcode dispatch (PARTIAL - see task log)"
