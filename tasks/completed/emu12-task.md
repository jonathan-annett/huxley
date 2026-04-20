## Task: EMU-12

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md`. The original source analysis is at `packages/emu86/docs/ORIGINAL-ANALYSIS.md`.

**You are working inside `packages/emu86/`.** All paths are relative to that directory.

### Previous tasks completed
- EMU-01 through EMU-11: Complete emulator core — decoder, all opcodes, run loop with batch execution and yield. 1316 assertions, all passing. Fibonacci(10)=55 verified.

### Your task

**Goal:** Build the Linux host — the platform implementation and CLI tool that makes `emu86` a runnable program. When this task is complete, you should be able to type:

```bash
./emu86 reference/bios test/images/freedos.img
```

and boot FreeDOS. Then:

```bash
./emu86 reference/bios test/images/fd1440-minix.img
```

and boot ELKs.

### What the Linux host does

The Linux host is a thin wrapper around the emulator core. It:

1. **Parses command-line arguments** — BIOS path, floppy image path, optional HD image path
2. **Loads disk images** into memory-mapped files or buffers
3. **Loads the BIOS** into the emulator state at F000:0100
4. **Initialises the lookup tables** from the BIOS data
5. **Sets up the platform interface:**
   - Disk read/write callbacks backed by the loaded images
   - Console I/O ring buffers backed by malloc'd memory
   - Terminal in raw, non-blocking mode for keyboard input
   - Timer via `clock_gettime(CLOCK_MONOTONIC)`
6. **Runs the main loop:** calls `emu86_run()` in a loop, servicing yields
7. **Handles terminal I/O:** reads keyboard into console_in, writes console_out to stdout
8. **Handles snapshots** (optional): save/load on signal or command-line flag
9. **Cleans up** on exit: restore terminal, close files

### Reference: how the original does it

The original 8086tiny's `runme.sh` does:

```bash
stty cbreak raw -echo min 0
./8086tiny bios fd.img
stty cooked echo
```

This puts the terminal in raw mode (character-at-a-time, no echo, non-blocking reads) before launching, and restores it after. Our host must do the equivalent programmatically using `termios`.

### Files to create

**`src/hosts/linux/main.c`** — CLI entry point

```c
int main(int argc, char *argv[])
{
    // 1. Parse args
    //    Usage: emu86 <bios> <floppy> [hd] [--snapshot-in file] [--snapshot-out file]
    //    The @ prefix on HD path means "boot from HD" (DL=0x80), matching 8086tiny convention

    // 2. Allocate Emu86State (large struct — may need to be heap-allocated or static)

    // 3. Initialise state: emu86_init(&state)

    // 4. Load BIOS into state->mem at F000:0100
    //    Read reference/bios (or specified path) into &state->mem[0xF0100]
    //    Set state->sregs[SREG_CS] = 0xF000
    //    Set state->ip = 0x0100

    // 5. Load lookup tables: emu86_load_tables(&tables, &state)

    // 6. Open disk images, store file descriptors for callbacks
    //    Set state->disk[N].size, cylinders, heads, sectors

    // 7. Set up platform: terminal, ring buffers, disk callbacks, timer

    // 8. If --snapshot-in, restore state from file (overrides init/BIOS load)

    // 9. Register signal handlers:
    //    SIGINT → save snapshot (if --snapshot-out), restore terminal, exit
    //    SIGTERM → same

    // 10. Set terminal to raw mode

    // 11. Main loop:
    //     while (running) {
    //         emu86_run(&state, &platform, &tables, CYCLE_BUDGET, &yield);
    //         switch (yield.reason) {
    //             case BUDGET: service_terminal(&platform); break;
    //             case IO_NEEDED: flush_console(&platform); break;
    //             case HALTED: wait_for_input(&platform); break;
    //             case EXIT: running = 0; break;
    //             case ERROR: report_error(); running = 0; break;
    //         }
    //     }

    // 12. If --snapshot-out, save snapshot

    // 13. Restore terminal, close files, exit
}
```

**`src/hosts/linux/platform_linux.c`** — Platform implementation

```c
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

// --- Terminal ---

static struct termios orig_termios;
static int terminal_raw = 0;

void terminal_init(void);     // Save original termios, set raw mode
void terminal_restore(void);  // Restore original termios
int terminal_read(void);      // Non-blocking read of one byte, returns -1 if none

// --- Disk ---

typedef struct {
    int fd;           // file descriptor
    uint32_t size;    // file size in bytes
} DiskImage;

static DiskImage disks[3];  // 0=HD, 1=FD, 2=BIOS

int platform_disk_read(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx);
int platform_disk_write(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx);

// --- Timer ---

uint64_t platform_get_time_us(void *ctx);

// --- Console service ---

// Poll terminal for keystrokes, write to console_in ring buffer
void service_keyboard(Emu86Platform *p);

// Flush console_out ring buffer to stdout
void flush_console(Emu86Platform *p);

// --- Ring buffer allocation ---

void alloc_ring_buffer(Emu86RingBuf *rb, uint32_t size);
void free_ring_buffer(Emu86RingBuf *rb);
```

**`src/hosts/linux/platform_linux.h`** — Header for the platform

**Terminal setup details:**

```c
void terminal_init(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    // Match 8086tiny's stty settings: cbreak raw -echo min 0
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;   // non-blocking
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    // Also set stdin to non-blocking via fcntl
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    terminal_raw = 1;
}
```

**Disk geometry calculation (for hard disk images):**

The 8086 BIOS expects CHS geometry stored in the state. The original calculates this from the file size. For standard floppy images:
- 1.44MB → 80 cylinders, 2 heads, 18 sectors
- 720KB → 80 cylinders, 2 heads, 9 sectors

For hard disk images, derive from size:
- Sectors per track = 63 (standard)
- Heads = 16
- Cylinders = size / (63 * 16 * 512)

**BIOS loading (match the original's init):**

```c
// Load BIOS binary into memory at F000:0100
int n = read(fd_bios, &state->mem[0xF0100], 0xFF00);

// The original also sets initial disk size in CX:AX for the BIOS to read:
// state->regs[REG_CX] = (hd_size >> 16) & 0xFFFF;
// state->regs[REG_AX] = hd_size & 0xFFFF;

// And sets DL for boot drive:
// DL = 0x00 for floppy, 0x80 for hard disk
```

Refer to ORIGINAL-ANALYSIS.md Section D (Initialization, lines 258-296) for the exact init sequence.

**Main loop details:**

```c
#define CYCLE_BUDGET 20000

while (running) {
    emu86_run(&state, &platform, &tables, CYCLE_BUDGET, &yield);

    switch (yield.reason) {
    case EMU86_YIELD_BUDGET:
        // Normal — flush output, poll input
        flush_console(&platform);
        service_keyboard(&platform);
        break;

    case EMU86_YIELD_IO_NEEDED:
        // Console output buffer filling up
        flush_console(&platform);
        break;

    case EMU86_YIELD_HALTED:
        // CPU halted — wait for keystroke or timer
        flush_console(&platform);
        // Use select() or poll() with a short timeout to wait for input
        // When input arrives (or timeout for timer), set int_pending
        {
            fd_set fds;
            struct timeval tv = { 0, 10000 }; // 10ms timeout
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                service_keyboard(&platform);
            }
            state.halted = 0;
            state.int_pending = 1;
            state.int_vector = 0x0A; // timer
        }
        break;

    case EMU86_YIELD_EXIT:
        running = 0;
        break;

    case EMU86_YIELD_ERROR:
        fprintf(stderr, "Emulator error at %04X:%04X\n",
                state.sregs[SREG_CS], state.ip);
        running = 0;
        break;
    }
}
```

**Snapshot support (CLI flags):**

```
./emu86 reference/bios test/images/freedos.img --snapshot-out snap.bin
  → On exit (Ctrl+C or QUITEMU), saves state to snap.bin

./emu86 --snapshot-in snap.bin reference/bios test/images/freedos.img
  → Restores state from snap.bin, uses disk images for I/O

./emu86 --snapshot-in snap.bin --snapshot-out snap.bin reference/bios test/images/freedos.img
  → Resume from snapshot, save updated snapshot on exit
```

### Files to create/modify

1. **`src/hosts/linux/main.c`** — CLI entry point
2. **`src/hosts/linux/platform_linux.c`** — Platform implementation
3. **`src/hosts/linux/platform_linux.h`** — Platform header
4. **Updated `Makefile`:**

```makefile
# Build the Linux CLI tool
emu86: src/emulator/run.c src/emulator/snapshot.c src/hosts/linux/main.c src/hosts/linux/platform_linux.c
	$(CC) $(CFLAGS) -o emu86 \
		src/hosts/linux/main.c \
		src/hosts/linux/platform_linux.c \
		src/emulator/run.c \
		src/emulator/snapshot.c \
		-Isrc/emulator -Isrc/hosts/linux

# Quick run targets for testing
run-freedos: emu86
	./emu86 reference/bios test/images/freedos.img

run-elks: emu86
	./emu86 reference/bios test/images/fd1440-minix.img
```

### Test plan

This task has both automated and manual tests. The automated tests verify the platform components. The manual tests verify boot.

**`test/unit/test_platform_linux.c`** — Automated tests

```
TEST: ring_buffer_alloc_free
  - alloc_ring_buffer with size 1024
  - Assert buf != NULL, size == 1024, *head == 0, *tail == 0
  - free_ring_buffer — no crash

TEST: disk_read_callback
  - Create a temp file with known content
  - Open as disk image
  - Call platform_disk_read at offset 0, len 512
  - Assert read data matches file content

TEST: disk_write_callback
  - Create a temp file
  - Call platform_disk_write with known data
  - Read file back, assert matches

TEST: disk_read_at_offset
  - Write known pattern at offset 1024 in temp file
  - platform_disk_read at offset 1024 → correct data

TEST: timer_returns_microseconds
  - Call platform_get_time_us twice with a small sleep between
  - Assert second call > first call
  - Assert difference is roughly correct (within 50%)

TEST: snapshot_save_load_file
  - Initialise state, set some register values
  - Save snapshot to a temp file (using emu86_snapshot_save + fwrite)
  - Clear state
  - Load snapshot from file (fread + emu86_snapshot_restore)
  - Assert registers match
```

**Manual boot tests** (document in test plan, verify interactively):

```
MANUAL TEST: FreeDOS boot
  1. make emu86
  2. ./emu86 reference/bios test/images/freedos.img
  3. Expect: FreeDOS boot messages, "A:\>" prompt
  4. Type "dir" → expect file listing
  5. Type "quitemu" → expect clean exit (or Ctrl+C)

MANUAL TEST: ELKs boot
  1. ./emu86 reference/bios test/images/fd1440-minix.img
  2. Expect: ELKs boot messages, "login:" prompt
  3. Type "root" → expect shell prompt "#"
  4. Type "ls /" → expect directory listing
  5. Type "ps" → expect process list
  6. Ctrl+C → exit (shutdown may hang, that's expected)

MANUAL TEST: Snapshot save and resume
  1. Boot FreeDOS: ./emu86 reference/bios test/images/freedos.img --snapshot-out /tmp/test.snap
  2. At A:\> prompt, type "echo hello"
  3. Ctrl+C to exit (saves snapshot)
  4. ./emu86 --snapshot-in /tmp/test.snap reference/bios test/images/freedos.img
  5. Expect: resumes at the same point (may need to press Enter to get prompt back)
```

**Semi-automated boot test:**

```bash
#!/bin/bash
# test/integration/test_boot_freedos.sh
# Send "dir" to FreeDOS via stdin, check output

timeout 10 ./emu86 reference/bios test/images/freedos.img <<'EOF' 2>/dev/null | tee /tmp/boot_output.txt
dir
quitemu
EOF

if grep -q "COMMAND" /tmp/boot_output.txt; then
    echo "PASS: FreeDOS booted and dir listing appeared"
else
    echo "FAIL: Expected dir listing not found"
    cat /tmp/boot_output.txt
    exit 1
fi
```

### Important implementation notes

1. **Emu86State is ~1.1MB** (the mem array alone is 0x110000 bytes). Allocate it on the heap with `malloc()` or as a `static` global — NOT on the stack (it'll overflow).

2. **The BIOS init sequence must match the original.** Load BIOS at 0xF0100, load tables, set CS:IP, set DL for boot drive, set CX:AX for HD size. If any of these are wrong, the BIOS won't boot. Refer to the original's init code (ORIGINAL-ANALYSIS.md Section D, Initialization).

3. **Terminal restore MUST happen on exit.** If the program crashes or is killed without restoring termios, the user's terminal is broken (no echo, raw mode). Use `atexit(terminal_restore)` as a safety net, and also restore in the signal handler.

4. **The console_out flush should handle ANSI escape sequences correctly.** The BIOS outputs ANSI escapes for cursor movement, clearing, etc. Just write the raw bytes to stdout — the host terminal handles the escapes. Do NOT try to interpret them.

5. **Non-blocking keyboard reads.** `terminal_read()` must return -1 immediately if no key is available. The emulator polls it between batches. Blocking reads would freeze the emulator.

6. **The BIOS uses custom opcodes (0x0F xx) for disk I/O and console output.** These are already handled by the opcode implementations in EMU-10 (exec_bios_putchar, exec_bios_disk_read, etc.) via the platform interface. The host just needs to provide working disk callbacks and ring buffers.

### Deliverables
1. `src/hosts/linux/main.c` — CLI entry point with arg parsing, init, main loop, cleanup
2. `src/hosts/linux/platform_linux.c` — Terminal, disk, timer, ring buffer implementations
3. `src/hosts/linux/platform_linux.h` — Platform header
4. Updated Makefile with `emu86`, `run-freedos`, `run-elks` targets
5. `test/unit/test_platform_linux.c` — Automated platform tests, passing
6. `test/integration/test_boot_freedos.sh` — Semi-automated boot test
7. All previous tests still pass (`make test-unit`)
8. **Manual verification: FreeDOS boots and ELKs boots**

### Rules
- `atexit(terminal_restore)` is mandatory — a broken terminal is unacceptable
- Emu86State must be heap-allocated or static, never stack-allocated
- The init sequence must match the original (BIOS at F0100, tables loaded, DL set, CX:AX for HD size)
- Console output is raw bytes to stdout — no interpretation of ANSI escapes
- Keyboard input is non-blocking — never block the emulator waiting for a keypress
- Disk callbacks must handle partial reads/writes and return error codes
- Snapshot files must be portable (they use the field-by-field format from EMU-02)
- Signal handler for SIGINT must restore terminal BEFORE exiting

### Post-completion checklist

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Build the emulator:**
   ```bash
   make emu86
   ```

3. **Manually verify FreeDOS boots:**
   ```bash
   make run-freedos
   # Type "dir" at A:\> prompt
   # Type "quitemu" to exit
   ```

4. **Manually verify ELKs boots (if image available):**
   ```bash
   make run-elks
   # Login as "root"
   # Type "ls /"
   # Ctrl+C to exit
   ```

5. **Run the semi-automated boot test:**
   ```bash
   bash test/integration/test_boot_freedos.sh
   ```

6. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## EMU-12
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X unit passed, Y failed}
   Boot tests: FreeDOS: PASS/FAIL, ELKs: PASS/FAIL
   Notes: {any issues, differences from reference build behaviour}
   ```

7. **If all tests pass AND both OSes boot:**
   ```bash
   cd ../../
   mv tasks/emu12-task.md tasks/completed/
   git add -A
   git commit -m "EMU-12: Linux host — FreeDOS and ELKs boot"
   git push origin master
   ```

8. **If automated tests pass but boot fails:**
   - Document what happens (crash? hang? garbled output?) in the task log
   - Compare behaviour with the reference build (`make reference-build`, then run the original)
   - Commit with: "EMU-12: Linux host (PARTIAL — boot issues, see task log)"
   - This is the most complex integration point. Boot failure is expected to require debugging.
