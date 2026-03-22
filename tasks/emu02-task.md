## Task: EMU-02

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md` (relative to the repo root). The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths in this task are relative to that directory unless explicitly stated otherwise.

### Previous tasks completed
- EMU-01: Reference source acquired, original source analysis complete, reference build verified (FreeDOS and ELKs boot successfully)

### Your task

**Goal:** Define the core data structures that the entire emulator builds on: `Emu86State` (the snapshotable machine state), `Emu86Platform` (the host abstraction interface), and the snapshot save/restore mechanism. Also define the `DecodeContext` struct for transient instruction decode state.

These structs are the architectural foundation. Every subsequent task depends on them being right. Use the global variable classification from `docs/ORIGINAL-ANALYSIS.md` as the primary input.

### Design constraints

Read `docs/emu86-roadmap.md` sections 2.2 through 2.5 for the full design rationale. Key points:

1. **`Emu86State` contains ALL persistent state.** Everything needed to stop the emulator, serialise the state to bytes, move it to a different machine, and resume execution. No globals. No pointers (they can't survive serialisation). No file descriptors.

2. **The state struct has explicit register fields, NOT memory-mapped registers.** The original 8086tiny stores registers inside the `mem` array at 0xF0000. We break that coupling. Registers are explicit `uint16_t` fields. The BIOS can still read/write them via I/O port emulation or a shadow mechanism, but the canonical register state is in the struct, not in memory.

3. **Scratch/transient values are NOT in the state struct.** They live in `DecodeContext` on the stack during execution. Refer to the "Scratch/Temporary" section of ORIGINAL-ANALYSIS.md — none of those variables belong in the state.

4. **`seg_override_en`, `rep_override_en`, `seg_override`, `rep_mode` DO belong in the state** even though they're decode-related — they span instruction boundaries (set by prefix, consumed by next instruction).

5. **`bios_table_lookup` is NOT in the state.** It's constant data derived from the BIOS image. It should be re-derived at init from the BIOS portion of memory, not snapshotted. Store it in a separate `Emu86Tables` struct that lives alongside the state but is not serialised.

6. **The platform interface uses ring buffers for all I/O.** Console in, console out, NIC TX, NIC RX — same structure everywhere. The ring buffer struct uses a plain buffer pointer plus volatile head/tail counters. On Linux these are malloc'd; in the browser they're SharedArrayBuffer views. The emulator code is identical either way.

7. **Snapshots are portable.** A snapshot taken in a browser must load on a Linux CLI and vice versa. This means: fixed-width types, explicit field order, canonical byte order (little-endian), version header, CRC32 checksum. No pointers serialised. No padding-dependent layout — serialise field by field, not with a raw memcpy of the struct.

8. **The disk state in the struct is geometry + seek position, not file descriptors.** The host provides disk data through platform callbacks. The state tracks what the guest OS thinks the disk looks like.

### Files to create

**`src/emulator/state.h`** — The Emu86State struct:

```
Suggested fields (adapt based on the analysis):

CPU:
  uint16_t regs[8]       — AX, CX, DX, BX, SP, BP, SI, DI
  uint16_t sregs[4]      — ES, CS, SS, DS  
  uint16_t ip
  uint16_t flags          — packed FLAGS register (NOT individual bytes like the original)
  uint8_t  halted         — HLT state
  uint8_t  trap_flag      — pending INT 1
  uint8_t  int_pending    — hardware interrupt waiting
  uint8_t  int_vector     — which interrupt

Prefix state (spans instructions):
  uint8_t  seg_override_en  — countdown (2→1→0)
  uint8_t  seg_override     — which segment register
  uint8_t  rep_override_en  — countdown (2→1→0)  
  uint8_t  rep_mode         — 0=REPNZ, 1=REPZ

Memory:
  uint8_t  mem[0x110000]   — 1MB + 64KB HMA

I/O:
  uint8_t  io_ports[0x10000]  — 64K I/O port space

Disk:
  struct { uint32_t size; uint16_t cylinders; uint8_t heads, sectors; uint32_t position; } disk[3]

Timer (PIT):
  struct { uint16_t reload; uint16_t counter; uint8_t mode; } pit[3]
  uint8_t  pit_lobyte_pending  — alternating hi/lo byte (was io_hi_lo)

Keyboard:
  uint8_t  kb_buffer[16]
  uint8_t  kb_head, kb_tail

Video (text mode):
  uint8_t  video_mode
  uint16_t cursor_row, cursor_col

Audio:
  uint8_t  spkr_en
  uint16_t wave_counter

Timing:
  uint64_t inst_count
  uint8_t  int8_asap        — pending timer interrupt

Graphics state:
  uint16_t graphics_x, graphics_y
```

Provide register index constants: `REG_AX=0, REG_CX=1, ..., REG_DI=7` and `SREG_ES=0, SREG_CS=1, SREG_SS=2, SREG_DS=3`.

Also provide flag bit constants: `FLAG_CF=0x0001, FLAG_PF=0x0004, FLAG_AF=0x0010, FLAG_ZF=0x0040, FLAG_SF=0x0080, FLAG_TF=0x0100, FLAG_IF=0x0200, FLAG_DF=0x0400, FLAG_OF=0x0800`.

Provide `emu86_init(Emu86State *state)` that zeroes everything and sets initial register values (CS=0xF000, IP=0x0100, matching the original's boot state).

**`src/emulator/decode.h`** — The DecodeContext struct:

All transient decode state. Lives on the stack. Never snapshotted.

```
  uint8_t   opcode
  uint8_t   xlat_id        — translated opcode (switch case number)
  uint8_t   extra          — sub-function index
  uint8_t   operand_width  — 0=byte, 1=word
  uint8_t   direction      — 0=rm←reg, 1=reg←rm
  uint8_t   mod, reg, rm   — ModRM fields
  uint32_t  rm_addr        — resolved effective address
  uint16_t  immediate
  uint16_t  displacement
  uint8_t   has_modrm      — whether this opcode uses ModRM
  uint8_t   inst_length    — total bytes consumed
  
  Scratch (was globals in original):
  uint32_t  op_source, op_dest, op_result
  uint32_t  op_to_addr, op_from_addr
  uint32_t  scratch_uint, scratch2_uint
  int32_t   scratch_int
  uint8_t   set_flags_type
```

**`src/emulator/platform.h`** — The Emu86Platform struct:

Ring buffer struct for I/O, disk callbacks, timer callback. See roadmap section 2.3 for the full interface.

The ring buffer struct:
```c
typedef struct {
    uint8_t  *buf;
    uint32_t  size;           // must be power of 2
    volatile uint32_t *head;  // producer writes here
    volatile uint32_t *tail;  // consumer reads here
} Emu86RingBuf;
```

Provide inline helpers:
```c
static inline uint32_t ringbuf_available(const Emu86RingBuf *rb);  // bytes available to read
static inline uint32_t ringbuf_free(const Emu86RingBuf *rb);       // space available to write
static inline int ringbuf_write(Emu86RingBuf *rb, uint8_t byte);   // write one byte, returns 0 on success
static inline int ringbuf_read(Emu86RingBuf *rb, uint8_t *byte);   // read one byte, returns 0 on success
static inline int ringbuf_write_buf(Emu86RingBuf *rb, const uint8_t *data, uint32_t len);
static inline int ringbuf_read_buf(Emu86RingBuf *rb, uint8_t *data, uint32_t len);
```

Platform struct:
```c
typedef struct {
    int (*disk_read)(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx);
    int (*disk_write)(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx);
    
    Emu86RingBuf console_out;   // emulator → host
    Emu86RingBuf console_in;    // host → emulator
    
    struct {
        Emu86RingBuf tx;        // emulator → host
        Emu86RingBuf rx;        // host → emulator
        uint8_t  mac[6];
        uint8_t  irq;
    } *nic;                     // NULL if no NIC attached
    
    uint64_t (*get_time_us)(void *ctx);
    void *ctx;                  // opaque host context
} Emu86Platform;
```

**`src/emulator/tables.h`** — The Emu86Tables struct:

```c
typedef struct {
    uint8_t data[20][256];   // the 20 lookup tables from the BIOS
} Emu86Tables;

// Load tables from the BIOS area in state->mem
// Call once after BIOS is loaded into memory
void emu86_load_tables(Emu86Tables *tables, const Emu86State *state);
```

Provide table index constants matching the analysis:
```
TABLE_XLAT_OPCODE, TABLE_XLAT_SUBFUNCTION, TABLE_STD_FLAGS, 
TABLE_PARITY_FLAG, TABLE_BASE_INST_SIZE, TABLE_I_W_SIZE, 
TABLE_I_MOD_SIZE, TABLE_COND_JUMP_DECODE_A/B/C/D, TABLE_FLAGS_BITFIELDS
```

**`src/emulator/snapshot.h` and `src/emulator/snapshot.c`** — Snapshot save/restore:

```c
#define EMU86_SNAPSHOT_MAGIC  0x53363845  // "E86S" in little-endian
#define EMU86_SNAPSHOT_VERSION 1

// Returns bytes written, or 0 on error (buffer too small)
uint32_t emu86_snapshot_save(const Emu86State *state, uint8_t *buf, uint32_t buf_size);

// Returns 0 on success, negative on error (-1 = bad magic, -2 = bad version, -3 = bad checksum, -4 = bad size)
int emu86_snapshot_restore(Emu86State *state, const uint8_t *buf, uint32_t buf_size);

// Returns the exact number of bytes needed for a snapshot
uint32_t emu86_snapshot_size(void);
```

Serialisation must be field-by-field in a defined order, little-endian, NOT a raw struct memcpy. This ensures portability across compilers and platforms.

The snapshot format:
```
[4 bytes] magic (EMU86_SNAPSHOT_MAGIC)
[4 bytes] version
[4 bytes] data size (everything after this header, before checksum)
[... field-by-field state data ...]
[4 bytes] CRC32 checksum of everything above
```

**`test/unit/harness.h`** — Minimal C test harness:

```c
// Simple test framework for C unit tests
#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); test_failures++; } else { test_passes++; } } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { printf("FAIL: %s:%d: %s != %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); test_failures++; } else { test_passes++; } } while(0)
#define RUN_TEST(name) do { printf("  %s...", #name); test_##name(); printf(" ok\n"); } while(0)

extern int test_passes;
extern int test_failures;
```

**`test/unit/test_snapshot.c`** — Snapshot tests:

Write the following tests:

```
TEST: snapshot_round_trip
  - Create Emu86State, set AX=0x1234, BX=0x5678, CS=0xF000, IP=0x0100
  - Write a known byte pattern to mem[0x100..0x1FF]
  - Set io_ports[0x300] = 0x42
  - Set inst_count = 1000000
  - Save snapshot to buffer
  - Zero out a second Emu86State
  - Restore from buffer
  - Assert all fields match: regs, sregs, ip, flags, mem pattern, io_ports, inst_count

TEST: snapshot_magic_check
  - Save valid snapshot
  - Corrupt magic bytes
  - Restore → assert returns -1

TEST: snapshot_version_check
  - Save valid snapshot  
  - Change version to 99
  - Restore → assert returns -2

TEST: snapshot_checksum_check
  - Save valid snapshot
  - Flip one bit in the middle of the data
  - Restore → assert returns -3

TEST: snapshot_size_deterministic
  - Create two identical states
  - Save both
  - Assert byte counts are equal
  - Assert buffers are byte-for-byte identical

TEST: snapshot_byte_order
  - Set AX = 0x1234
  - Save snapshot
  - Find the AX field in the buffer (after header)
  - Assert first byte is 0x34 (little-endian low byte)
  - Assert second byte is 0x12 (little-endian high byte)

TEST: snapshot_size_query
  - Call emu86_snapshot_size()
  - Assert it returns a positive value
  - Assert it equals the actual bytes written by emu86_snapshot_save()

TEST: snapshot_buffer_too_small
  - Call emu86_snapshot_save with a buffer smaller than needed
  - Assert returns 0 (error)
```

**`test/unit/test_ringbuf.c`** — Ring buffer tests:

```
TEST: ringbuf_write_read_single
  - Write one byte, read it back, assert match

TEST: ringbuf_write_read_multiple  
  - Write 10 bytes, read 10 back, assert all match in order

TEST: ringbuf_empty_read_fails
  - Fresh buffer, read → assert failure (returns -1)

TEST: ringbuf_full_write_fails
  - Fill buffer completely
  - Write one more → assert failure

TEST: ringbuf_wraps_around
  - Write enough to wrap past the buffer end
  - Read all back, assert correct order

TEST: ringbuf_available_and_free
  - Write 5 bytes to a 16-byte buffer
  - Assert available() == 5
  - Assert free() == 10 (size-1 minus written, since one slot is sentinel)

TEST: ringbuf_bulk_write_read
  - Write a 100-byte block, read it back as a block
  - Assert byte-for-byte match
```

**Update the `Makefile`** to add:
```
test-snapshot: compile and run test/unit/test_snapshot.c
test-ringbuf: compile and run test/unit/test_ringbuf.c
test-unit: run all unit tests
```

### Deliverables
1. `src/emulator/state.h` — Emu86State struct with init function
2. `src/emulator/decode.h` — DecodeContext struct
3. `src/emulator/platform.h` — Emu86Platform struct with ring buffer helpers
4. `src/emulator/tables.h` — Emu86Tables struct with table loading
5. `src/emulator/snapshot.h` — Snapshot function declarations
6. `src/emulator/snapshot.c` — Snapshot implementation (field-by-field serialisation)
7. `test/unit/harness.h` — C test harness macros
8. `test/unit/test_snapshot.c` — Snapshot tests (all passing)
9. `test/unit/test_ringbuf.c` — Ring buffer tests (all passing)
10. Updated Makefile with test targets
11. All tests passing: `make test-unit`

### Rules
- All types must be fixed-width (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`) — no `int`, `short`, `unsigned` for state fields
- No pointers in `Emu86State` — it must be serialisable
- The snapshot serialisation must NOT use `memcpy(&state, buf, sizeof(state))` — it must write/read field by field for portability
- Ring buffer size must be a power of 2 (use `& (size-1)` for wrapping, not modulo)
- All header files must have include guards
- Commit from repo root with message: "EMU-02: Core state structs, platform interface, and snapshot"
- Run all tests before committing: `make test-unit`
