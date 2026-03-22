# emu86 — Emulator Sub-Roadmap

## A clean-room refactor of 8086tiny into a portable, snapshotable, maintainable 8086 emulator

**Status:** Ready to start  
**Priority:** Phase 0 — before the editor  
**Source material:** [adriancable/8086tiny](https://github.com/adriancable/8086tiny) (MIT License)  
**Output:** A standalone emulator that runs identically on Linux CLI and in a browser Web Worker

---

## 1. Design Philosophy

We are not porting 8086tiny. We are using it as a reference implementation to build a clean, maintainable, production-grade 8086 emulator. The original is brilliant engineering — a full PC XT in under 25K of C — but it achieves that through macro density and variable overloading that makes it impenetrable to anyone who didn't write it. We're going the other direction.

**Principles:**

- **Readability over cleverness.** Every opcode gets a descriptive function name. No macro gymnastics. A developer reading `execute_add_reg_rm()` should understand what it does without cross-referencing a lookup table.
- **State is explicit and contained.** All CPU and machine state lives in a single `Emu86State` struct. No globals that mix ephemeral scratch values with persistent state. Temporary values are clearly named and scoped.
- **The emulator is a library, not a program.** There is no `main()` in the emulator. There is a `emu86_step()` function that advances the machine by one instruction. The host (Linux CLI or browser Worker) calls it in a loop. The host owns the clock.
- **Platform abstraction at the boundary.** The emulator talks to the outside world through a small set of function pointers and shared memory regions. The Linux host and the browser Worker implement the same interface. A snapshot taken in the browser loads on Linux and vice versa.
- **Snapshots are the unit of debugging.** If something goes wrong in the browser, download the snapshot, load it in the CLI tool, step through it instruction by instruction. Deterministic replay.

---

## 2. Architecture

### 2.1 Core Model

```
┌──────────────────────────────────────────────────────┐
│                    Host (either)                      │
│                                                      │
│  ┌─── Linux CLI ───┐    ┌─── Browser Worker ───┐    │
│  │ main()          │    │ onmessage()          │    │
│  │   while(run) {  │    │   while(run) {       │    │
│  │     emu86_step  │    │     emu86_step       │    │
│  │   }             │    │   }                  │    │
│  └────────┬────────┘    └────────┬─────────────┘    │
│           │                      │                   │
│           └──────────┬───────────┘                   │
│                      │                               │
│              ┌───────▼────────┐                      │
│              │  Platform API  │                      │
│              │  (identical)   │                      │
│              └───────┬────────┘                      │
└──────────────────────┼───────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────┐
│              emu86 core (pure C, no OS calls)         │
│                                                      │
│  emu86_step(state, platform)                         │
│       │                                              │
│       ├── decode instruction                         │
│       ├── execute (descriptive function per opcode)  │
│       ├── handle interrupts                          │
│       └── service I/O via platform callbacks         │
│                                                      │
│  Emu86State (single struct, fully snapshotable)      │
│       ├── cpu: registers, flags, IP, segments        │
│       ├── memory[1MB + 64KB]                         │
│       ├── io_ports[0x10000]                          │
│       ├── disk: drive state, geometry                │
│       ├── timer: pit counters, irq state             │
│       ├── keyboard: scan code buffer                 │
│       └── video: mode, cursor pos (text mode only)   │
│                                                      │
│  No globals. No OS calls. No SDL. Pure computation.  │
└──────────────────────────────────────────────────────┘
```

### 2.2 The State Struct

Everything the emulator needs to stop, save, move to a different machine, and resume:

```c
typedef struct {
    // --- CPU ---
    uint16_t regs[8];          // AX, CX, DX, BX, SP, BP, SI, DI
    uint16_t sregs[4];         // ES, CS, SS, DS
    uint16_t ip;
    uint16_t flags;
    
    // --- Memory ---
    uint8_t  mem[0x110000];    // 1MB + 64KB HMA
    
    // --- I/O ports ---
    uint8_t  io_ports[0x10000];
    
    // --- Interrupt state ---
    uint8_t  int_pending;
    uint8_t  int_vector;
    uint8_t  trap_flag;
    uint8_t  halted;
    
    // --- Disk ---
    struct {
        uint32_t  size;        // image size in bytes
        uint16_t  cylinders;
        uint8_t   heads;
        uint8_t   sectors;
    } disk[3];                 // 0=HD, 1=FD, 2=BIOS (matches 8086tiny)
    
    // --- PIT (timer) ---
    struct {
        uint16_t  reload;
        uint16_t  counter;
        uint8_t   mode;
    } pit[3];
    
    // --- Keyboard ---
    uint8_t  kb_buffer[16];
    uint8_t  kb_head;
    uint8_t  kb_tail;
    
    // --- Video (text mode) ---
    uint8_t  video_mode;
    uint16_t cursor_row;
    uint16_t cursor_col;
    
    // --- Instruction counter (for timing, debugging) ---
    uint64_t inst_count;
    
} Emu86State;
```

**What is NOT in this struct:** scratch values used during instruction decoding and execution. Those live on the stack in local variables inside `emu86_run()` and the opcode functions. The struct is *only* persistent state — everything needed to snapshot and resume.

### 2.3 The Platform Interface

The emulator never calls the OS. Instead, the host provides a platform struct with function pointers and shared memory pointers:

```c
typedef struct {
    // --- Disk I/O ---
    // Read/write sectors from disk images.
    // The host decides where images live (files on Linux, ArrayBuffers in browser).
    int (*disk_read)(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx);
    int (*disk_write)(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx);
    
    // --- Console I/O ---
    // Ring buffers for text input/output.
    // On Linux: backed by a terminal or pipe.
    // In browser: backed by SharedArrayBuffer regions readable by the main thread.
    struct {
        uint8_t  *buf;         // ring buffer memory
        uint32_t  size;        // buffer size (power of 2)
        volatile uint32_t *head;  // write position (producer increments)
        volatile uint32_t *tail;  // read position (consumer increments)
    } console_out;             // emulator → host (character output)
    
    struct {
        uint8_t  *buf;
        uint32_t  size;
        volatile uint32_t *head;
        volatile uint32_t *tail;
    } console_in;              // host → emulator (keyboard input)
    
    // --- Network adapter (virtual NIC) ---
    // Same ring buffer model. Ethernet frames in/out.
    // NULL if no NIC attached.
    struct {
        uint8_t  *tx_buf;     // emulator → host (outbound frames)
        uint32_t  tx_size;
        volatile uint32_t *tx_head;
        volatile uint32_t *tx_tail;
        
        uint8_t  *rx_buf;     // host → emulator (inbound frames)
        uint32_t  rx_size;
        volatile uint32_t *rx_head;
        volatile uint32_t *rx_tail;
        
        uint8_t   mac[6];     // MAC address
        uint8_t   irq;        // IRQ line to assert on rx
    } *nic;
    
    // --- Timer ---
    // Returns microseconds since some epoch. Monotonic.
    uint64_t (*get_time_us)(void *ctx);
    
    // --- Host context ---
    // Opaque pointer passed to all callbacks. On Linux, points to file handles etc.
    // In browser, points to whatever the Worker needs.
    void *ctx;
    
} Emu86Platform;
```

**The key insight:** the ring buffers use the same memory layout everywhere. On Linux, they're `malloc`'d regions with atomic head/tail pointers. In the browser, they're views into `SharedArrayBuffer` with `Atomics` operations on the head/tail. The emulator code doesn't know the difference — it reads `*head`, writes `*tail`, accesses `buf[offset & (size-1)]`. The host is responsible for the other end of each ring buffer.

### 2.4 The Run Function — Batch Execution

The emulator does not expose a single-step function across the WASM boundary. Crossing the JS ↔ WASM boundary millions of times per second wastes cycles on bookkeeping. Instead, `emu86_run()` executes a batch of instructions internally (defaulting to ~20,000 cycles) and only returns when it hits the cycle budget or needs something from the host.

```c
// Why the emulator yielded control back to the host
typedef struct {
    int      reason;
    uint32_t cycles_used;   // how many cycles consumed this batch
    uint8_t  io_type;       // for IO_NEEDED: what kind
    uint16_t io_port;       // for IO_NEEDED: which port
} Emu86YieldInfo;

#define EMU86_YIELD_BUDGET     0  // hit the cycle limit, nothing special needed
#define EMU86_YIELD_HALTED     1  // HLT instruction, waiting for interrupt
#define EMU86_YIELD_IO_NEEDED  2  // console_out buffer full, NIC tx, etc
#define EMU86_YIELD_BREAKPOINT 3  // hit a debug breakpoint
#define EMU86_YIELD_ERROR      4  // fatal: bad opcode, triple fault

// Run until we exhaust the budget or need host attention.
void emu86_run(Emu86State *state, Emu86Platform *platform,
               uint32_t cycle_budget, Emu86YieldInfo *yield);
```

The tight loop inside `emu86_run()`:

```c
void emu86_run(Emu86State *s, Emu86Platform *p,
               uint32_t budget, Emu86YieldInfo *yield) {
    uint32_t cycles = 0;

    while (cycles < budget) {
        // Check pending hardware interrupts
        if (s->int_pending && (s->flags & FLAG_IF)) {
            service_interrupt(s);
        }

        if (s->halted) {
            yield->reason = EMU86_YIELD_HALTED;
            yield->cycles_used = cycles;
            return;
        }

        // Decode + execute — all inline, no boundary crossings
        cycles += execute_instruction(s, p);

        // Check if an I/O op needs host attention
        // (console_out buffer nearly full, NIC tx queued, etc.)
        if (io_needs_flush(p)) {
            yield->reason = EMU86_YIELD_IO_NEEDED;
            yield->cycles_used = cycles;
            return;
        }
    }

    yield->reason = EMU86_YIELD_BUDGET;
    yield->cycles_used = budget;
}
```

**Everything between the `while` and the `return` is inline C.** The compiler sees one large function body. No WASM boundary crossings, no function call overhead on the hot path. `execute_instruction()` and all opcode handlers are `static inline` — the compiler folds them into this loop.

The host calls `emu86_run()` once per batch:

```c
// Linux host — tight loop
while (running) {
    emu86_run(&state, &platform, 20000, &yield);

    switch (yield.reason) {
    case EMU86_YIELD_BUDGET:
        // Flush console output, check for input, service timers
        flush_console(&platform);
        poll_keyboard(&platform);
        break;
    case EMU86_YIELD_IO_NEEDED:
        flush_console(&platform);
        break;
    case EMU86_YIELD_HALTED:
        // Wait for interrupt (keyboard, timer)
        wait_for_event(&platform);
        state.int_pending = 1;
        break;
    case EMU86_YIELD_ERROR:
        fprintf(stderr, "Fatal emulator error at %04X:%04X\n",
                state.sregs[SREG_CS], state.ip);
        running = 0;
        break;
    }
}
```

```typescript
// Browser Worker — yield to message pump between batches
function runBatch() {
    _emu86_run(statePtr, platformPtr, CYCLE_BUDGET, yieldPtr);

    const reason = readYieldReason(yieldPtr);

    if (reason === YIELD_IO_NEEDED || reason === YIELD_BUDGET) {
        // Flush console_out ring buffer contents to main thread
        flushConsoleOutput();
    }

    // Process messages accumulated while we were in WASM
    // (keystrokes pushed to console_in, snapshot requests, etc.)
    drainMessageQueue();

    if (running) setTimeout(runBatch, 0);
}
```

**The 20,000-cycle budget is the key tuning parameter.** It's long enough that the emulator does real work per batch (a typical instruction is 4–20 cycles, so ~1,000–5,000 instructions per batch). It's short enough that the Worker yields frequently for message processing — keystrokes arrive with <1ms perceived latency. The value is configurable per host.

**Console output buffering:** The emulator doesn't yield on every `putchar`. The console_out ring buffer accumulates characters. The `io_needs_flush()` check only triggers when the buffer hits ~75% capacity. A command like `ls` that produces 40 lines yields maybe 2–3 times as the buffer fills, not 2,000 times for 2,000 characters. The host flushes the ring buffer to its destination (stdout on Linux, postMessage to xterm.js in the browser) between batches.

**Keyboard input:** On the browser, the Worker's `onmessage` handler writes scancodes directly into the console_in ring buffer (calling a small exported WASM function, or writing directly into the SharedArrayBuffer). The emulator picks them up on its next pass through the interrupt check at the top of the loop. The cycle budget yield is what gives the Worker its window to drain the message queue and push those keys.

### 2.5 Snapshot Model

A snapshot is the `Emu86State` struct serialised to bytes. That's it.

```c
// Write the entire state to a byte buffer. Returns size written.
uint32_t emu86_snapshot_save(const Emu86State *state, uint8_t *buf, uint32_t buf_size);

// Restore state from a byte buffer. Returns 0 on success.
int emu86_snapshot_restore(Emu86State *state, const uint8_t *buf, uint32_t buf_size);
```

The snapshot includes a version header and a checksum. It does NOT include disk images (those are large and stable — the host provides them separately). The snapshot captures the machine mid-instruction-boundary: IP points to the next instruction, all registers and memory reflect the completed state.

**Portable by construction:** the struct has fixed-width types, explicit field order, and no pointers. A snapshot saved from a browser `SharedArrayBuffer` loads byte-for-byte identical on a Linux CLI. The `emu86_snapshot_save` function writes in a canonical byte order (little-endian, matching x86 convention) regardless of host endianness.

### 2.6 The Virtual NIC

Rather than emulating a specific historical NIC (NE2000, etc.) which requires writing an ELKs driver, we implement a paravirtualised device. This is the same approach virtio uses: the guest knows it's in a VM, and cooperates with the host through shared memory regions and a simple interrupt protocol.

The NIC uses two I/O ports and a shared memory region:

```
I/O port 0x300: Command/status register
I/O port 0x301: Data register
Shared memory at D000:0000 (conventional memory, 64KB)
    Offset 0x0000: TX ring descriptor (head, tail, entry size)
    Offset 0x0010: RX ring descriptor (head, tail, entry size)
    Offset 0x0100: TX buffer (frame data)
    Offset 0x8000: RX buffer (frame data)
```

The protocol:

1. Guest writes a frame to the TX buffer, updates TX head
2. Guest writes `CMD_TX_NOTIFY` to port 0x300
3. Host reads from the TX ring (via platform ring buffer), processes/routes the frame
4. Host writes incoming frame to RX buffer, updates RX head
5. Host asserts IRQ on the NIC's interrupt line
6. Guest's ISR reads from RX ring, processes the frame

On the host side, the NIC's ring buffers map directly to the platform `nic` struct. On Linux, the host could bridge to a TAP device for real networking, or to a userspace TCP/IP stack for loopback. In the browser, the host bridges to the editor's transport layer.

An ELKs driver for this paravirtualised NIC is ~200 lines of C. We write it as part of this project and include it in the ELKs disk image.

---

## 3. Refactoring Strategy for 8086tiny.c

The original `8086tiny.c` is a single file with massive macros that encode, decode, and execute instructions through preprocessor expansion. We refactor it into readable functions, keeping the original as a behaviour reference.

### 3.1 Opcode Refactoring — Inline Functions, Not Regular Functions

The original uses macros for performance — zero call overhead. We keep that property by using `static inline` functions. This is a hard rule throughout the codebase:

**If it was a macro in the original, it becomes `static inline`, not a regular function.**

At `-O2` (our minimum optimisation level for both gcc and emcc), the compiler inlines these exactly as if they were macros, but we get type checking, debuggability, and readability. In debug builds (`-O0`), they become real function calls with proper stack traces — a significant debugging advantage over macros.

```c
// Original macro — dense, unreadable, no type safety:
#define R_M_OP(dest,op,src) (i_w ? op_dest = CAST(unsigned short)dest, \
    op_result = CAST(unsigned short)dest op \
    (op_source = CAST(unsigned short)src) : ...)

// Refactored — readable, typed, same codegen at -O2:
static inline void exec_add_rm_reg(Emu86State *s, DecodeContext *d) {
    uint32_t rm_val = read_rm(s, d);
    uint32_t reg_val = read_reg(s, d);
    uint32_t result = rm_val + reg_val;
    
    set_flags_add(s, rm_val, reg_val, result, d->operand_width);
    write_rm(s, d, result);
}
```

For critical-path helpers (flag setting, memory reads, register access), we use `__attribute__((always_inline))` as belt-and-suspenders to guarantee the compiler never generates a call:

```c
static inline __attribute__((always_inline)) 
uint16_t read_reg16(const Emu86State *s, uint8_t reg) {
    return s->regs[reg];
}
```

All opcode functions and helpers live in headers included by the single translation unit that contains `emu86_run()`. This guarantees the compiler has full visibility for inlining. The final compiled output is effectively one large function body — just like the original macro-expanded code, but written by humans who can read it.

Each opcode family gets its own header (for guaranteed inline visibility) and a corresponding test file:

```
src/emulator/opcodes/
├── arithmetic.h     # ADD, SUB, ADC, SBB, CMP, NEG, MUL, DIV, INC, DEC
├── logic.h          # AND, OR, XOR, NOT, TEST
├── shift.h          # SHL, SHR, SAR, ROL, ROR, RCL, RCR
├── transfer.h       # MOV, PUSH, POP, XCHG, LEA, LDS, LES
├── string.h         # MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W, REP
├── control.h        # JMP, CALL, RET, INT, IRET, conditional jumps
├── flags.h          # CLC, STC, CLI, STI, CLD, STD, CMC, LAHF, SAHF
├── io.h             # IN, OUT
├── misc.h           # NOP, HLT, XLAT, CBW, CWD, AAA, AAS, DAA, DAS
└── helpers.h        # read_rm, write_rm, read_reg, write_reg, set_flags_*

src/emulator/
├── decode.h         # DecodeContext, decode_instruction (inline)
├── run.c            # emu86_run() — the ONE .c file that includes all headers
│                    # Compiler sees entire emulator as one translation unit
├── snapshot.c       # save/restore (separate TU, not on hot path)
├── state.h          # Emu86State struct
├── platform.h       # Emu86Platform struct, ring buffer types
└── devices/
    └── nic.h        # Virtual NIC I/O port handlers (inline)
```

**Critical build detail:** `run.c` is the single compilation unit for the hot path. It `#include`s all the opcode headers, `decode.h`, `helpers.h`, and `nic.h`. The compiler sees everything and inlines aggressively. This is intentional — it's the same "unity build" technique used in game engines for exactly this reason. The opcodes are in separate *files* for human readability, but they compile as one unit for machine performance.

### 3.2 Decode Context

Instead of scattered globals, instruction decoding produces a local struct:

```c
typedef struct {
    uint8_t   opcode;          // raw opcode byte
    uint8_t   operand_width;   // 0 = byte, 1 = word
    uint8_t   direction;       // 0 = rm←reg, 1 = reg←rm
    uint8_t   mod;             // ModR/M mod field
    uint8_t   reg;             // ModR/M reg field
    uint8_t   rm;              // ModR/M r/m field
    uint32_t  rm_addr;         // resolved memory address (or register index)
    uint16_t  immediate;       // immediate operand if present
    uint16_t  displacement;    // displacement if present
    uint8_t   segment_override; // 0xFF = none, else segment register index
    uint8_t   rep_prefix;      // 0 = none, 1 = REP, 2 = REPNZ
    uint8_t   inst_length;     // total bytes consumed
} DecodeContext;
```

All temporaries. Lives on the stack. Never snapshotted.

### 3.3 What We Keep From the Original

- The BIOS binary and its assembly source (it's correct, well-structured, and works)
- The instruction encoding tables (the opcode-to-function mapping is accurate)
- The interrupt handling logic (correctly implements the 8086 interrupt priority model)
- The disk geometry calculations

### 3.4 What We Strip

- SDL graphics (all of it — we're text mode only)
- The `KEYBOARD_DRIVER` and `SDL_KEYBOARD_DRIVER` macros (replaced by ring buffer reads)
- Audio/PC speaker support (not needed)
- Direct POSIX I/O calls (replaced by platform callbacks)
- CGA/Hercules graphics memory handling (keep the memory map, strip the rendering)

---

## 4. Build Tasks

### Phase 0A — Setup & Comprehension (day 1)

#### `EMU-01`: Repository setup and source annotation

**Do:**
1. Create the `emu86/` project directory with the structure from Section 4 of this doc
2. Copy `8086tiny.c` and `bios.asm` into `reference/` (read-only — never modified, used for comparison)
3. Read `8086tiny.c` line by line. Produce `docs/ORIGINAL-ANALYSIS.md` documenting:
   - Every global variable, classified as CPU state / machine state / scratch
   - Every macro, with a plain-English description of what it does
   - Every point where the code touches the OS (file I/O, SDL, stdin)
   - The main loop structure and instruction dispatch mechanism
4. Set up the build system: Makefile for Linux (gcc), Makefile.emscripten for WASM (emcc)
5. Set up vitest for the TypeScript wrapper tests, and a simple C test harness for the core

**Test plan:**

```
TEST: Reference build
  - Compile original 8086tiny.c with gcc (no SDL, text mode)
  - Boot a FreeDOS floppy image
  - Type "dir" at the prompt
  - Assert output appears (manual for now, automated later)

TEST: Documentation completeness
  - ORIGINAL-ANALYSIS.md lists every global variable
  - ORIGINAL-ANALYSIS.md lists every macro
  - ORIGINAL-ANALYSIS.md lists every OS call
```

---

### Phase 0B — Core State & Decode (days 2–4)

#### `EMU-02`: Define Emu86State and Emu86Platform structs

**Do:**
1. Create `src/emulator/state.h` with the `Emu86State` struct
2. Create `src/emulator/platform.h` with the `Emu86Platform` struct
3. Create `src/emulator/snapshot.c` — save/restore state to/from byte buffer
4. Write the canonical serialisation format (little-endian, versioned header, CRC32 checksum)

**Test file: `test/unit/snapshot.test.c`**

```
TEST: Snapshot round-trip
  - Create Emu86State, set known register values and memory patterns
  - Save to buffer
  - Clear state
  - Restore from buffer
  - Assert all registers and memory match

TEST: Snapshot version header
  - Save a snapshot
  - Assert first 4 bytes are magic number "E86S"
  - Assert next 4 bytes are version number

TEST: Snapshot checksum
  - Save a snapshot
  - Corrupt one byte in the middle
  - Attempt restore → assert failure (checksum mismatch)

TEST: Snapshot size is deterministic
  - Create identical states
  - Save both
  - Assert byte-for-byte identical

TEST: Cross-platform byte order
  - Save snapshot with known values
  - Assert specific bytes at known offsets match expected little-endian encoding
  - (This ensures a snapshot from browser loads on Linux)
```

#### `EMU-03`: Instruction decoder

**Do:**
1. Create `src/emulator/decode.c` — fetch bytes from memory, decode ModR/M, resolve addresses
2. Create `src/emulator/decode.h` — `DecodeContext` struct, decoder function declarations
3. Implement `decode_instruction(Emu86State *s, DecodeContext *d)` — fills in the decode context from the instruction at CS:IP
4. Implement address resolution: `resolve_rm_address()`, `read_rm()`, `write_rm()`, `read_reg()`, `write_reg()`

**Test file: `test/unit/decode.test.c`**

```
TEST: Decode MOV AX, 1234h
  - Place bytes [B8 34 12] at CS:IP
  - Decode → opcode = MOV_REG_IMM, reg = AX, immediate = 0x1234

TEST: Decode ADD [BX+SI+10h], AL
  - Place appropriate bytes at CS:IP
  - Decode → mod=01, rm uses BX+SI+displacement, operand_width=byte

TEST: Segment override prefix
  - Place ES: prefix + MOV instruction
  - Decode → segment_override = ES

TEST: REP prefix
  - Place REP MOVSB
  - Decode → rep_prefix = REP, opcode = MOVSB

TEST: Instruction length calculation
  - Decode various instructions
  - Assert inst_length matches expected byte count
```

---

### Phase 0C — Opcode Implementation (days 5–10)

One task per opcode family. Each task produces a `.c` file with named functions and a test file.

#### `EMU-04`: Arithmetic opcodes

`src/emulator/opcodes/arithmetic.c` — ADD, ADC, SUB, SBB, CMP, NEG, INC, DEC, MUL, IMUL, DIV, IDIV

**Test approach:** For each opcode, set up registers/memory with known values, execute, verify result and all affected flags (CF, ZF, SF, OF, AF, PF). Use the reference `8086tiny.c` output as the expected values for edge cases (e.g., overflow, divide by zero).

```
TEST: ADD byte sets carry flag on overflow
TEST: ADD word sets zero flag
TEST: SUB sets borrow flag correctly
TEST: CMP does not modify destination
TEST: MUL 8-bit produces 16-bit result in AX
TEST: MUL 16-bit produces 32-bit result in DX:AX
TEST: DIV by zero triggers INT 0
TEST: INC does not affect carry flag
TEST: NEG of zero sets ZF, clears CF
... (30-40 tests per family)
```

#### `EMU-05`: Logic opcodes
#### `EMU-06`: Shift/rotate opcodes
#### `EMU-07`: Data transfer opcodes (MOV, PUSH, POP, XCHG, LEA, LDS, LES)
#### `EMU-08`: String opcodes (MOVS, CMPS, STOS, LODS, SCAS + REP)
#### `EMU-09`: Control flow opcodes (JMP, CALL, RET, INT, IRET, conditional jumps, LOOP)
#### `EMU-10`: Flag, I/O, and miscellaneous opcodes

Each follows the same pattern: named functions, comprehensive flag tests, reference comparison.

---

### Phase 0D — Integration: Stepping & Interrupts (days 11–12)

#### `EMU-11`: The run function, interrupt handling, and I/O yield logic

**Do:**
1. Create `src/emulator/run.c` — the `emu86_run()` batch execution loop
2. Wire up the opcode dispatch (opcode → inline function). All opcode headers included in this translation unit.
3. Implement interrupt handling: hardware IRQs, software INT, NMI, priority
4. Implement PIT timer (counter decrement, IRQ 0 firing based on cycles consumed)
5. Implement keyboard interrupt (IRQ 1 from console_in ring buffer)
6. Implement `io_needs_flush()` — checks console_out fill level, NIC tx pending
7. Also expose `emu86_step_single()` — runs exactly one instruction (for debugger/test use, NOT for production hot path). This can be a regular function since it's only used in tooling.

**Test file: `test/unit/run.test.c`**

```
TEST: Run executes instructions and returns cycle count
  - Place a sequence of NOPs at CS:IP
  - Run with budget=100
  - Assert yield.reason == BUDGET
  - Assert yield.cycles_used > 0

TEST: Run yields on HLT
  - Place HLT instruction at CS:IP
  - Run with budget=20000
  - Assert yield.reason == HALTED
  - Assert yield.cycles_used < 20000

TEST: Run yields when console_out buffer fills
  - Place a loop that writes characters via INT 10h
  - Use a small console_out buffer (64 bytes)
  - Run with budget=100000
  - Assert yield.reason == IO_NEEDED at some point before budget exhausted

TEST: INT 21h triggers interrupt handler
  - Set up IVT entry for INT 21h
  - Execute INT 21h instruction
  - Assert CS:IP jumped to handler address

TEST: Hardware IRQ fires when IF=1
  - Set int_pending, IF=1
  - Run one batch
  - Assert interrupt was serviced (IP changed to handler)

TEST: Hardware IRQ deferred when IF=0
  - Set int_pending, IF=0 (CLI)
  - Run one batch
  - Assert interrupt NOT serviced
  - Set IF=1 (STI)
  - Run another batch
  - Assert interrupt now serviced

TEST: PIT counter fires IRQ 0 at correct interval
TEST: Keyboard scancode from ring buffer delivered via INT 9

TEST: emu86_step_single executes exactly one instruction
  - Place MOV AX, 1234h at CS:IP
  - Call step_single
  - Assert AX == 0x1234
  - Assert IP advanced by exactly 3 bytes
```

---

### Phase 0E — Linux Host (days 13–14)

#### `EMU-12`: Linux platform implementation and CLI tool

**Do:**
1. Create `src/hosts/linux/platform_linux.c` — implements `Emu86Platform`:
   - Disk I/O via `mmap`'d files
   - Console ring buffers backed by `malloc` with terminal raw mode
   - Timer via `clock_gettime(CLOCK_MONOTONIC)`
   - NIC: optional, backed by TUN/TAP or loopback
2. Create `src/hosts/linux/main.c`:
   - Parse args: BIOS image, floppy image, hard disk image, snapshot file
   - Initialise state (or restore from snapshot)
   - Run the step loop
   - Handle signals (Ctrl+C → snapshot and exit)
3. Create `src/hosts/linux/terminal.c` — raw terminal I/O, feeds console_in ring buffer from stdin, reads console_out to stdout

**Test file: `test/integration/boot-freedos.test.sh`**

```bash
#!/bin/bash
# Boot FreeDOS, send "dir\r", capture output, check for expected strings

timeout 10 ./emu86 -bios bios.bin -fd test/images/freedos.img <<'INPUT' > output.txt
dir
INPUT

grep -q "COMMAND" output.txt && echo "PASS: FreeDOS booted" || echo "FAIL"
grep -q "COM" output.txt && echo "PASS: dir listing appeared" || echo "FAIL"
```

**Test file: `test/integration/snapshot-cli.test.sh`**

```bash
#!/bin/bash
# Boot, set a value, snapshot, restore, verify value persists

# Boot and set AX via debug.com or a test program
timeout 5 ./emu86 -bios bios.bin -fd test/images/freedos.img \
  --snapshot-out /tmp/test.snap --run-for 1000000

# Restore and verify
./emu86 --snapshot-in /tmp/test.snap --dump-regs > regs.txt

# Check instruction counter is preserved
grep -q "inst_count:" regs.txt && echo "PASS: snapshot restored" || echo "FAIL"
```

---

### Phase 0F — ELKs Boot & Network (days 15–17)

#### `EMU-13`: Boot ELKs, verify shell works

**Do:**
1. Obtain or build an ELKs hard disk image with a basic toolchain
2. Boot it on the Linux CLI host
3. Verify: login prompt appears, shell commands work, files can be created
4. Document any BIOS modifications needed for ELKs compatibility

**Tests:**

```
TEST: ELKs boot to login prompt
TEST: Login as root
TEST: "ls /" produces expected directories
TEST: "echo hello > /tmp/test && cat /tmp/test" round-trips
TEST: Basic C compilation (if toolchain present)
```

#### `EMU-14`: Virtual NIC implementation

**Do:**
1. Create `src/emulator/devices/nic.c` — the paravirtualised NIC device
   - I/O port handlers for 0x300-0x301
   - Shared memory region at D000:0000
   - TX: guest writes frame → host reads from platform nic tx ring
   - RX: host writes frame to platform nic rx ring → IRQ fires → guest reads
2. Create `src/hosts/linux/nic_loopback.c` — loopback adapter for testing
3. Write the ELKs driver: `elks-driver/emu86_nic.c` (~200 lines)
4. Build an ELKs image with the driver installed

**Tests:**

```
TEST: Guest writes TX frame, host receives it via ring buffer
TEST: Host writes RX frame, guest receives interrupt and reads it
TEST: Ping loopback (if ELKs has ping and the driver works)
TEST: Frame integrity — send 1500-byte frame, verify no corruption
TEST: Back-to-back frames don't drop
```

---

### Phase 0G — Browser Host (days 18–20)

#### `EMU-15`: Emscripten compilation

**Do:**
1. Create `Makefile.emscripten` — compile the emulator core to WASM
   - Flags: `-O2 -s MODULARIZE=1 -s EXPORT_ES6=1 -s ALLOW_MEMORY_GROWTH=1 -s ENVIRONMENT=worker -s NO_EXIT_RUNTIME=1`
   - Export: `_emu86_init`, `_emu86_step`, `_emu86_snapshot_save`, `_emu86_snapshot_restore`
   - No filesystem emulation (we handle I/O ourselves)
2. Create `src/hosts/browser/platform_browser.c` — implements `Emu86Platform`:
   - Console ring buffers backed by SharedArrayBuffer views
   - Disk I/O from ArrayBuffers (loaded from compressed images)
   - Timer via imported JS `performance.now()`
   - NIC ring buffers backed by SharedArrayBuffer

**Tests:**

```
TEST: WASM module compiles without errors
TEST: Exported functions are accessible from JS
TEST: Module instantiation with SharedArrayBuffer works
```

#### `EMU-16`: Web Worker wrapper

**Do:**
1. Create `src/hosts/browser/worker.ts` — the Worker entry point:
   - Load WASM module
   - Set up SharedArrayBuffer regions for ring buffers
   - Run step loop in batches (yield every ~10ms to check messages)
   - Handle messages: start, stop, load-image, snapshot, restore, resize, keypress
2. Create `src/hosts/browser/controller.ts` — main-thread API:
   - `start(bios, diskImage)` — launch Worker, send images, begin execution
   - `sendKey(scancode)` — write to console_in ring buffer
   - `onOutput(callback)` — read from console_out ring buffer
   - `snapshot()` → `Promise<ArrayBuffer>` — request and receive snapshot
   - `restore(snapshot)` — send snapshot to Worker
   - `stop()` — halt execution
3. Create `src/hosts/browser/messages.ts` — message type definitions

**Test file: `test/integration/browser-boot.test.ts`**

```
TEST: Worker starts and begins execution
  - Create controller, load BIOS + FreeDOS image
  - Start emulator
  - Wait for console output
  - Assert output contains boot messages

TEST: Keyboard input reaches guest
  - Boot FreeDOS
  - Send "dir\r" as keystrokes
  - Assert output contains directory listing

TEST: Snapshot from browser loads on CLI
  - Boot in browser Worker
  - Take snapshot
  - Save to file
  - Load in CLI tool (via Node child_process)
  - Dump registers
  - Assert instruction counter > 0

TEST: Restore snapshot in browser
  - Boot, take snapshot at time T1
  - Continue running to time T2
  - Restore T1 snapshot
  - Assert state matches T1 (instruction counter reset)
```

---

### Phase 0H — Package & Integration Test (days 21–22)

#### `EMU-17`: npm package and demo

**Do:**
1. Create `package.json` for `@lite-editor/emu86`
2. Bundle: WASM binary, Worker script, Controller, types
3. Create a minimal demo page: xterm.js connected to the controller
4. Build the compressed ELKs disk image with toolchain
5. Write the README

**Test: End-to-end demo**

```
TEST: Demo page boots ELKs
  - Open demo.html in browser
  - Assert: ELKs login prompt appears in xterm
  - Type "root", press Enter
  - Assert: shell prompt appears
  - Type "ls /", press Enter
  - Assert: directory listing appears

TEST: Full snapshot portability
  - Boot in browser, run for 30 seconds
  - Download snapshot
  - Load snapshot in CLI: ./emu86 --snapshot-in downloaded.snap
  - Assert: emulator resumes at exact same point
  - Run "echo $?" in shell
  - Assert: output matches expected state
```

---

## 5. Task Execution Order

```
Phase 0A — Setup
  1. EMU-01    Source annotation & build setup

Phase 0B — Core
  2. EMU-02    State & Platform structs, snapshot
  3. EMU-03    Instruction decoder

Phase 0C — Opcodes
  4. EMU-04    Arithmetic
  5. EMU-05    Logic
  6. EMU-06    Shift/rotate
  7. EMU-07    Data transfer
  8. EMU-08    String operations
  9. EMU-09    Control flow
  10. EMU-10   Flags, I/O, misc

Phase 0D — Integration
  11. EMU-11   Step function, interrupt handling, PIT, keyboard

Phase 0E — Linux Host
  12. EMU-12   Platform implementation, CLI tool, terminal

Phase 0F — ELKs & NIC
  13. EMU-13   ELKs boot verification
  14. EMU-14   Virtual NIC + ELKs driver

Phase 0G — Browser Host
  15. EMU-15   Emscripten compilation
  16. EMU-16   Web Worker + Controller

Phase 0H — Package
  17. EMU-17   npm package, demo, docs
```

**Estimated total: ~22 working days**

The opcode phase (0C) is the longest but highly parallelisable — each opcode family is independent. If multiple agents work in parallel, this phase compresses significantly.

---

## 6. Relationship to the Editor

The `emu86` module is consumed by the editor as an npm dependency. The integration points:

| Editor component | emu86 interface | Purpose |
|-----------------|----------------|---------|
| Terminal panel (ELKs tab) | `Controller.start()`, `sendKey()`, `onOutput()` | Run ELKs in a terminal tab |
| Time machine | `Controller.snapshot()`, `Controller.restore()` | Checkpoint WASM state to warm storage |
| AI tool pipeline | NIC ring buffers | Deploy compiled tools from ELKs to editor |
| REPL | `Controller.stop()`, `Controller.start()` | Start/stop ELKs instance |

The editor never touches the emulator internals. It talks to the Controller, which talks to the Worker, which talks to the WASM module. Clean boundary.

---

## 7. Non-Goals for This Phase

- **Graphics mode:** We strip SDL and CGA/Hercules rendering entirely. Text mode only. If someone wants to run Alley Cat in the browser, that's a different project.
- **Sound:** PC speaker emulation is stripped. 
- **Mouse:** Not needed for ELKs.
- **Performance optimisation beyond -O2:** The emulator will be fast enough. 8086 code on modern hardware runs at effectively infinite speed. We optimise for correctness and maintainability, not nanoseconds per instruction.
- **Full NIC protocol stack in the emulator:** The emulator provides the ring buffers and interrupts. The TCP/IP stack runs inside ELKs. The host side routes frames — that's the editor's job, not the emulator's.
