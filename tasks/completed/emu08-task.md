## Task: EMU-08

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-07: All core structs, decoder, arithmetic, logic, shift/rotate, data transfer opcodes. 775 assertions, all passing.

### Your task

**Goal:** Implement the string operation family: MOVSB/MOVSW, CMPSB/CMPSW, STOSB/STOSW, LODSB/LODSW, SCASB/SCASW, plus the REP/REPNZ/REPZ prefix handling.

String operations are the 8086's bulk data primitives. They operate on memory at DS:SI (source) and ES:DI (destination), auto-incrementing or decrementing the index registers based on the direction flag (DF). Combined with REP prefixes, they become hardware-assisted loops.

### The string operations

All string ops share a common pattern:
1. Perform one operation using DS:SI and/or ES:DI
2. Increment or decrement SI and/or DI by 1 (byte) or 2 (word), based on DF:
   - DF=0: increment (forward)
   - DF=1: decrement (backward)

**MOVSB/MOVSW (A4/A5) — Move String**
- `mem[ES:DI] = mem[DS:SI]`
- Advance SI and DI
- No flags affected
- With REP: repeat CX times (block memory copy)

**CMPSB/CMPSW (A6/A7) — Compare String**
- `temp = mem[DS:SI] - mem[ES:DI]` (subtract, set flags, discard result)
- Advance SI and DI
- Sets CF, PF, AF, ZF, SF, OF (like SUB)
- With REPZ: repeat while equal (ZF=1) and CX > 0 — find first difference
- With REPNZ: repeat while not equal (ZF=0) and CX > 0 — find first match

**STOSB/STOSW (AA/AB) — Store String**
- `mem[ES:DI] = AL` (byte) or `mem[ES:DI] = AX` (word)
- Advance DI only (no SI involvement)
- No flags affected
- With REP: fill memory block (like memset)

**LODSB/LODSW (AC/AD) — Load String**
- `AL = mem[DS:SI]` (byte) or `AX = mem[DS:SI]` (word)
- Advance SI only (no DI involvement)
- No flags affected
- REP is technically valid but rarely useful (just loads the last value)

**SCASB/SCASW (AE/AF) — Scan String**
- `temp = AL - mem[ES:DI]` (byte) or `temp = AX - mem[ES:DI]` (word)
- Advance DI only
- Sets flags like SUB (CF, PF, AF, ZF, SF, OF)
- With REPZ: scan while equal (ZF=1) — find first non-matching byte
- With REPNZ: scan while not equal (ZF=0) — find first matching byte (like strchr)

### REP prefix handling

The REP/REPZ/REPNZ prefixes (already decoded by EMU-03 into `state->rep_override_en` and `state->rep_mode`) cause string operations to repeat with CX as the counter.

The repeat logic (this will be called from the run loop in EMU-11, but implement the core here):

```c
// Execute one iteration of a string operation.
// Returns: 1 if the REP loop should continue, 0 if done.
// The caller (run loop) handles decrementing CX and checking the loop condition.

static inline int exec_string_op_once(Emu86State *s, DecodeContext *d);
```

However, for efficiency the actual implementation should handle the full REP loop internally when possible, since yielding after every single byte of a 64KB MOVSB is too slow:

```c
// Execute a string operation, handling REP internally.
// For non-REP: executes once.
// For REP MOVSB/STOSB/LODSB: loops until CX=0 (no flag check needed).
// For REPZ CMPSB/SCASB: loops until CX=0 or ZF=0.
// For REPNZ CMPSB/SCASB: loops until CX=0 or ZF=1.
static inline void exec_string_op(Emu86State *s, DecodeContext *d);
```

The REP loop for each operation:
1. If CX == 0, do nothing (exit immediately)
2. Execute one iteration
3. CX--
4. For REP (MOVS/STOS/LODS): if CX > 0, goto 2
5. For REPZ (CMPS/SCAS): if CX > 0 AND ZF=1, goto 2
6. For REPNZ (CMPS/SCAS): if CX > 0 AND ZF=0, goto 2

### Index advancement helper

Create a reusable helper for SI/DI advancement:

```c
// Advance an index register (SI or DI) by operand size, respecting DF
static inline void index_advance(Emu86State *s, uint8_t reg, uint8_t width) {
    int delta = (width == 0) ? 1 : 2;
    if (get_flag(s, FLAG_DF))
        s->regs[reg] -= delta;
    else
        s->regs[reg] += delta;
}
```

### Files to create

**`src/emulator/opcodes/string.h`**

```c
static inline void index_advance(Emu86State *s, uint8_t reg, uint8_t width);

// Individual string operations (one iteration)
static inline void exec_movsb(Emu86State *s);
static inline void exec_movsw(Emu86State *s);
static inline void exec_cmpsb(Emu86State *s);
static inline void exec_cmpsw(Emu86State *s);
static inline void exec_stosb(Emu86State *s);
static inline void exec_stosw(Emu86State *s);
static inline void exec_lodsb(Emu86State *s);
static inline void exec_lodsw(Emu86State *s);
static inline void exec_scasb(Emu86State *s);
static inline void exec_scasw(Emu86State *s);

// REP-aware dispatcher
static inline void exec_string_op(Emu86State *s, DecodeContext *d);
```

**`test/unit/test_string.c`**

```
=== Index advancement ===

TEST: index_advance_forward_byte
  - DF=0, SI=0x100, advance SI byte → SI=0x101

TEST: index_advance_forward_word
  - DF=0, SI=0x100, advance SI word → SI=0x102

TEST: index_advance_backward_byte
  - DF=1, SI=0x100, advance SI byte → SI=0x0FF

TEST: index_advance_backward_word
  - DF=1, SI=0x100, advance SI word → SI=0x0FE

=== MOVSB/MOVSW ===

TEST: movsb_single
  - DS:SI points to byte 0x42, ES:DI points to zeroed memory
  - MOVSB → mem[ES:DI] = 0x42, SI += 1, DI += 1

TEST: movsw_single
  - DS:SI points to word 0x1234
  - MOVSW → mem[ES:DI] = 0x1234, SI += 2, DI += 2

TEST: movsb_backward
  - DF=1, MOVSB → SI -= 1, DI -= 1

TEST: movsb_no_flags
  - Set known flags, MOVSB, assert unchanged

TEST: rep_movsb_block_copy
  - Set up 10 bytes at DS:SI, CX=10, REP MOVSB
  - Assert all 10 bytes copied to ES:DI region
  - Assert CX=0, SI advanced by 10, DI advanced by 10

TEST: rep_movsb_cx_zero
  - CX=0, REP MOVSB → nothing happens, SI and DI unchanged

TEST: rep_movsw_block_copy
  - Set up 5 words at DS:SI, CX=5, REP MOVSW
  - Assert all 5 words copied
  - Assert CX=0

=== CMPSB/CMPSW ===

TEST: cmpsb_equal
  - DS:SI and ES:DI both point to 0x42
  - CMPSB → ZF=1, CF=0

TEST: cmpsb_less
  - DS:SI=0x10, ES:DI=0x20
  - CMPSB → ZF=0, CF=1 (source < dest)

TEST: cmpsb_greater
  - DS:SI=0x20, ES:DI=0x10
  - CMPSB → ZF=0, CF=0

TEST: repz_cmpsb_find_difference
  - DS:SI = "ABCDE", ES:DI = "ABCXE", CX=5
  - REPZ CMPSB → stops at index 3 (D vs X)
  - CX=2 (5 - 3 iterations), ZF=0

TEST: repz_cmpsb_all_equal
  - DS:SI = "AAAA", ES:DI = "AAAA", CX=4
  - REPZ CMPSB → CX=0, ZF=1

TEST: repnz_cmpsb_find_match
  - DS:SI = "ABCD", ES:DI = "XXCX", CX=4
  - REPNZ CMPSB → stops at index 2 (C matches C)
  - ZF=1

=== STOSB/STOSW ===

TEST: stosb_single
  - AL=0x42, ES:DI=addr
  - STOSB → mem[addr]=0x42, DI += 1

TEST: stosw_single
  - AX=0x1234, STOSW → word stored, DI += 2

TEST: rep_stosb_fill
  - AL=0xFF, CX=16, REP STOSB
  - Assert 16 bytes filled with 0xFF, CX=0

TEST: stosb_no_flags
  - Assert flags unchanged

=== LODSB/LODSW ===

TEST: lodsb_single
  - mem[DS:SI]=0x42
  - LODSB → AL=0x42, SI += 1

TEST: lodsw_single
  - mem[DS:SI]=0x1234
  - LODSW → AX=0x1234, SI += 2

TEST: lodsb_no_flags
  - Assert flags unchanged

=== SCASB/SCASW ===

TEST: scasb_match
  - AL=0x42, mem[ES:DI]=0x42
  - SCASB → ZF=1

TEST: scasb_no_match
  - AL=0x42, mem[ES:DI]=0x99
  - SCASB → ZF=0

TEST: repnz_scasb_find_byte
  - AL=0x42, ES:DI points to [0x00, 0x00, 0x42, 0x00], CX=4
  - REPNZ SCASB → stops at index 2 (found 0x42)
  - CX=2, ZF=1

TEST: repnz_scasb_not_found
  - AL=0xFF, ES:DI points to [0x00, 0x00, 0x00, 0x00], CX=4
  - REPNZ SCASB → CX=0, ZF=0

TEST: repz_scasb_scan_while_equal
  - AL=0x42, ES:DI points to [0x42, 0x42, 0x42, 0x99], CX=4
  - REPZ SCASB → stops at index 3 (found non-0x42)
  - CX=1, ZF=0

=== Segment override with string ops ===

TEST: movsb_with_segment_override
  - Set seg_override to ES (overrides the DS:SI source)
  - MOVSB should use ES:SI instead of DS:SI
  - Note: ES:DI destination is always ES, never overridden
```

### Deliverables
1. `src/emulator/opcodes/string.h` — All string operation implementations
2. `test/unit/test_string.c` — All tests listed above, passing
3. Updated Makefile with `test-string` target
4. All previous tests still pass (`make test-unit`)

### Rules
- All functions `static inline`
- MOVS, STOS, LODS do NOT affect flags
- CMPS and SCAS set flags exactly like SUB (use `set_flags_sub` from helpers.h)
- The REP loop must check CX == 0 BEFORE the first iteration (CX=0 means do nothing)
- For REPZ CMPS/SCAS: continue while ZF=1 AND CX > 0
- For REPNZ CMPS/SCAS: continue while ZF=0 AND CX > 0
- The ES:DI destination segment is ALWAYS ES, even with a segment override prefix. Only the DS:SI source can be overridden.
- Direction flag: DF=0 means forward (increment), DF=1 means backward (decrement)

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-08
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../
   mv tasks/emu08-task.md tasks/completed/
   git add -A
   git commit -m "EMU-08: String opcodes"
   git push origin master
   ```

4. **If any tests fail that you cannot resolve:**
   - Document in the task log with `Status: PARTIAL` and details
   - Commit message: "EMU-08: String opcodes (PARTIAL - see task log)"
