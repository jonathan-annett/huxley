## Task: EMU-09

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-08: All core structs, decoder, arithmetic, logic, shift/rotate, data transfer, string opcodes. All tests passing.

### Your task

**Goal:** Implement the control flow opcode family: JMP, CALL, RET, INT, IRET, conditional jumps (Jcc), LOOP/LOOPZ/LOOPNZ, JCXZ.

These instructions modify CS:IP to redirect execution. They're critical for program structure — every branch, function call, interrupt handler, and loop depends on them.

### The control flow operations

**JMP — Unconditional Jump**
- JMP short rel8 (EB) — IP += sign-extended 8-bit offset
- JMP near rel16 (E9) — IP += 16-bit offset
- JMP far ptr16:16 (EA) — CS:IP = immediate far pointer
- JMP near r/m16 (FF/4) — IP = r/m value
- JMP far m16:16 (FF/5) — CS:IP = far pointer from memory
- No flags affected

**CALL — Call Procedure**
- CALL near rel16 (E8) — push IP (of next instruction), IP += 16-bit offset
- CALL far ptr16:16 (9A) — push CS, push IP, load new CS:IP
- CALL near r/m16 (FF/2) — push IP, IP = r/m value
- CALL far m16:16 (FF/3) — push CS, push IP, CS:IP from memory
- No flags affected
- Uses `stack_push()` from EMU-07

**RET — Return from Procedure**
- RET near (C3) — pop IP
- RET near imm16 (C2) — pop IP, then SP += imm16 (discard parameters)
- RETF (CB) — pop IP, pop CS
- RETF imm16 (CA) — pop IP, pop CS, then SP += imm16
- No flags affected
- Uses `stack_pop()` from EMU-07

**INT — Software Interrupt**
- INT imm8 (CD) — push FLAGS, push CS, push IP, clear IF and TF, load CS:IP from IVT
- INT 3 (CC) — same as INT 3 but single-byte encoding (breakpoint)
- INTO (CE) — INT 4 if OF=1, otherwise no-op
- The Interrupt Vector Table (IVT) is at memory address 0000:0000, with 4 bytes per vector (2 bytes IP, 2 bytes CS)

**IRET — Return from Interrupt**
- Pop IP, pop CS, pop FLAGS
- Restores all flags (like POPF)
- Uses `stack_pop()` and `flags_unpack()` from EMU-07

**Conditional Jumps (Jcc) — 70-7F**
- All are short jumps: IP += sign-extended 8-bit offset, IF condition is true
- No flags affected

The conditions (use the BIOS tables TABLE_COND_JUMP_DECODE_A/B/C/D or implement directly):

| Opcode | Mnemonic | Condition |
|--------|----------|-----------|
| 70 | JO | OF=1 |
| 71 | JNO | OF=0 |
| 72 | JB/JNAE/JC | CF=1 |
| 73 | JNB/JAE/JNC | CF=0 |
| 74 | JZ/JE | ZF=1 |
| 75 | JNZ/JNE | ZF=0 |
| 76 | JBE/JNA | CF=1 OR ZF=1 |
| 77 | JNBE/JA | CF=0 AND ZF=0 |
| 78 | JS | SF=1 |
| 79 | JNS | SF=0 |
| 7A | JP/JPE | PF=1 |
| 7B | JNP/JPO | PF=0 |
| 7C | JL/JNGE | SF != OF |
| 7D | JNL/JGE | SF == OF |
| 7E | JLE/JNG | ZF=1 OR (SF != OF) |
| 7F | JNLE/JG | ZF=0 AND (SF == OF) |

**LOOP/LOOPZ/LOOPNZ/JCXZ (E0-E3)**
- LOOPNZ/LOOPNE (E0) — CX--, jump if CX != 0 AND ZF=0
- LOOPZ/LOOPE (E1) — CX--, jump if CX != 0 AND ZF=1
- LOOP (E2) — CX--, jump if CX != 0
- JCXZ (E3) — jump if CX == 0 (no CX decrement)
- All use short rel8 offset
- LOOP decrements CX BEFORE testing. If CX was 0, it wraps to 0xFFFF and the loop continues (this is 8086 behaviour).
- No flags affected by the LOOP instruction itself (CX decrement does not set flags)

### Important note on IP advancement

The control flow instructions set IP directly. The normal IP advancement (that the run loop does after every instruction based on `inst_length`) must be SKIPPED or overridden for instructions that modify IP. There are two approaches:

**Approach A:** The exec function sets IP directly. The run loop checks a flag in DecodeContext (e.g. `d->ip_changed = 1`) and skips the normal IP += inst_length.

**Approach B:** The exec function sets IP to the final value including the effect of inst_length (i.e., all relative jumps add to `IP + inst_length`, not to `IP`).

**Use Approach A** — it's clearer. Add a field to DecodeContext:
```c
uint8_t ip_changed;  // set to 1 by control flow instructions
```

When `ip_changed` is set, the run loop uses `state->ip` as-is instead of adding `inst_length`.

For relative jumps, the offset is relative to the IP of the NEXT instruction (IP + inst_length), not the current instruction. So:
```c
state->ip = state->ip + d->inst_length + (int8_t)offset;
```

### The `pc_interrupt` helper

Create a helper for the INT instruction sequence (also used by hardware interrupts in EMU-11):

```c
// Execute an interrupt: push FLAGS, push CS:IP, load vector, clear IF+TF
// vector_num: interrupt number (0-255)
// The IVT is at linear address 0x00000, 4 bytes per entry
static inline void pc_interrupt(Emu86State *s, uint8_t vector_num);
```

Implementation:
1. Push FLAGS (using `flags_pack` and `stack_push`)
2. Push CS
3. Push IP (this should be the IP of the NEXT instruction, i.e., current IP + inst_length for software INT, or current IP for hardware interrupts)
4. Clear IF and TF in FLAGS
5. Load IP from `mem[vector_num * 4]` (16-bit, little-endian)
6. Load CS from `mem[vector_num * 4 + 2]` (16-bit, little-endian)

### Files to create

**`src/emulator/opcodes/control.h`**

```c
// Interrupt helper (used by INT instruction and hardware interrupt delivery)
static inline void pc_interrupt(Emu86State *s, uint8_t vector_num);

// Conditional jump evaluation
static inline int eval_condition(const Emu86State *s, uint8_t condition_code);

// Jump instructions
static inline void exec_jmp_short(Emu86State *s, DecodeContext *d);
static inline void exec_jmp_near(Emu86State *s, DecodeContext *d);
static inline void exec_jmp_far(Emu86State *s, DecodeContext *d);
static inline void exec_jmp_rm(Emu86State *s, DecodeContext *d);
static inline void exec_jmp_far_mem(Emu86State *s, DecodeContext *d);

// Conditional jumps
static inline void exec_jcc(Emu86State *s, DecodeContext *d);

// CALL
static inline void exec_call_near(Emu86State *s, DecodeContext *d);
static inline void exec_call_far(Emu86State *s, DecodeContext *d);
static inline void exec_call_rm(Emu86State *s, DecodeContext *d);
static inline void exec_call_far_mem(Emu86State *s, DecodeContext *d);

// RET
static inline void exec_ret_near(Emu86State *s, DecodeContext *d);
static inline void exec_ret_near_imm(Emu86State *s, DecodeContext *d);
static inline void exec_retf(Emu86State *s, DecodeContext *d);
static inline void exec_retf_imm(Emu86State *s, DecodeContext *d);

// INT / IRET
static inline void exec_int(Emu86State *s, DecodeContext *d, uint8_t vector);
static inline void exec_int3(Emu86State *s, DecodeContext *d);
static inline void exec_into(Emu86State *s, DecodeContext *d);
static inline void exec_iret(Emu86State *s, DecodeContext *d);

// LOOP
static inline void exec_loop(Emu86State *s, DecodeContext *d);
static inline void exec_loopz(Emu86State *s, DecodeContext *d);
static inline void exec_loopnz(Emu86State *s, DecodeContext *d);
static inline void exec_jcxz(Emu86State *s, DecodeContext *d);
```

**`test/unit/test_control.c`**

```
=== Conditional jump evaluation ===

TEST: eval_jz_true
  - ZF=1, eval_condition(JZ) → 1

TEST: eval_jz_false
  - ZF=0, eval_condition(JZ) → 0

TEST: eval_jb_true
  - CF=1, eval_condition(JB) → 1

TEST: eval_ja_true
  - CF=0 AND ZF=0, eval_condition(JA) → 1

TEST: eval_ja_false_cf
  - CF=1, eval_condition(JA) → 0

TEST: eval_ja_false_zf
  - ZF=1, eval_condition(JA) → 0

TEST: eval_jl_true
  - SF=1, OF=0 (SF != OF), eval_condition(JL) → 1

TEST: eval_jl_false
  - SF=1, OF=1 (SF == OF), eval_condition(JL) → 0

TEST: eval_jle_true_zf
  - ZF=1, eval_condition(JLE) → 1

TEST: eval_jle_true_sf_of
  - SF=0, OF=1, ZF=0, eval_condition(JLE) → 1

TEST: eval_jg_true
  - ZF=0, SF=0, OF=0, eval_condition(JG) → 1

TEST: eval_all_16_conditions
  - Systematically test all 16 condition codes with appropriate flag states

=== JMP ===

TEST: jmp_short_forward
  - IP=0x100, offset=+5 → IP=0x100 + inst_length + 5
  - Assert ip_changed flag set

TEST: jmp_short_backward
  - IP=0x100, offset=-10 (signed) → IP=0x100 + inst_length - 10

TEST: jmp_near
  - IP=0x100, offset=+0x200 → IP adjusted accordingly

TEST: jmp_far
  - JMP 0x2000:0x0100 → CS=0x2000, IP=0x0100

TEST: jmp_rm
  - BX=0x0500, JMP BX → IP=0x0500

TEST: jmp_no_flags
  - Set flags, JMP, assert unchanged

=== Jcc ===

TEST: jcc_taken
  - Set ZF=1, JZ offset=+10 → IP advances by 10 (plus inst_length)
  - Assert ip_changed

TEST: jcc_not_taken
  - Set ZF=0, JZ offset=+10 → IP NOT changed (normal inst_length advance)
  - Assert ip_changed NOT set

=== CALL / RET ===

TEST: call_near_and_ret
  - IP=0x100, CALL near offset to 0x200
  - Assert IP=0x200, return address (0x100 + inst_length) pushed on stack
  - RET → IP = 0x100 + inst_length (the return address)

TEST: call_far_and_retf
  - CS=0x1000, IP=0x100, CALL FAR 0x2000:0x300
  - Assert CS=0x2000, IP=0x300, old CS and IP pushed
  - RETF → CS=0x1000, IP=0x100 + inst_length

TEST: ret_imm_pops_extra
  - CALL near, push some extra words on stack
  - RET 4 → IP restored AND SP adjusted by 4 additional bytes

TEST: call_rm
  - BX=0x0500, CALL BX → push return address, IP=0x0500

=== INT / IRET ===

TEST: int_pushes_flags_cs_ip
  - Set up IVT entry for INT 0x21 at address 0x84 (0x21 * 4)
  - Write handler address: IP=0x1000, CS=0x0000
  - Execute INT 0x21
  - Assert: FLAGS, old CS, old IP pushed on stack (3 words)
  - Assert: CS=0x0000, IP=0x1000
  - Assert: IF=0, TF=0

TEST: int_iret_roundtrip
  - Set known flags (CF=1, IF=1, DF=1)
  - Execute INT 0x21
  - Assert IF=0 (cleared by INT)
  - Execute IRET
  - Assert CS, IP restored to original next-instruction values
  - Assert FLAGS restored (CF=1, IF=1, DF=1)

TEST: int3_uses_vector_3
  - Set up IVT for vector 3
  - INT 3 → jumps to vector 3 handler

TEST: into_triggers_when_of_set
  - OF=1, INTO → triggers INT 4

TEST: into_noop_when_of_clear
  - OF=0, INTO → no effect, IP advances normally

TEST: int_clears_if_and_tf
  - IF=1, TF=1, INT 0x21 → IF=0, TF=0

=== LOOP ===

TEST: loop_decrements_cx
  - CX=5, LOOP offset → CX=4, jump taken

TEST: loop_exits_when_cx_zero
  - CX=1, LOOP → CX=0, jump NOT taken

TEST: loop_cx_wraps
  - CX=0, LOOP → CX=0xFFFF, jump taken (8086 behaviour: decrement before test)

TEST: loopz_continues_while_zf
  - CX=5, ZF=1, LOOPZ → CX=4, jump taken

TEST: loopz_exits_on_zf_clear
  - CX=5, ZF=0, LOOPZ → CX=4, jump NOT taken

TEST: loopnz_continues_while_not_zf
  - CX=5, ZF=0, LOOPNZ → CX=4, jump taken

TEST: loopnz_exits_on_zf_set
  - CX=5, ZF=1, LOOPNZ → CX=4, jump NOT taken

TEST: jcxz_jumps_when_zero
  - CX=0, JCXZ offset → jump taken

TEST: jcxz_no_jump_when_nonzero
  - CX=1, JCXZ → jump NOT taken

TEST: loop_no_flags_affected
  - Set known flags, LOOP, assert CX decremented but flags unchanged
```

Also add `ip_changed` to DecodeContext if not already present (update `src/emulator/decode.h`).

### Deliverables
1. `src/emulator/opcodes/control.h` — All control flow implementations
2. `test/unit/test_control.c` — All tests listed above, passing
3. Updated `src/emulator/decode.h` if needed (ip_changed field)
4. Updated Makefile with `test-control` target
5. All previous tests still pass (`make test-unit`)

### Rules
- All functions `static inline`
- Relative jumps are relative to the NEXT instruction's IP (IP + inst_length), not current IP
- INT must push FLAGS, CS, IP in that order, then clear IF and TF
- IRET must pop IP, CS, FLAGS in that order (reverse of INT's push)
- LOOP decrements CX BEFORE testing — CX=0 wraps to 0xFFFF and loops
- Conditional jumps do NOT affect flags
- LOOP does NOT affect flags (the CX decrement is silent)
- `pc_interrupt()` will be reused by hardware interrupt delivery in EMU-11 — make it a clean standalone helper
- The `ip_changed` mechanism must be clear and documented — the run loop depends on it

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-09
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu09-task.md tasks/completed/
   git add -A
   git commit -m "EMU-09: Control flow opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-09: Control flow opcodes (PARTIAL - see task log)"
