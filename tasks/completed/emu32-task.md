# EMU-32: Keyboard input via named pipe, synchronised across both emulators

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

With EMU-31 landed, the harness can split output to two named pipes (`/tmp/emu86-harness/ref.out` and `.../hux.out`) for visible lockstep observation. The emulator reaches the FreeDOS A:\> prompt. The natural next step is keyboard input — so the user (or a script) can type commands and see both emulators respond identically.

This task adds a third named pipe, `/tmp/emu86-harness/kbd.in`, that accepts keyboard bytes. The harness reads from it and delivers each byte to both emulators *synchronously at the same simulated step*, preserving lockstep.

### The determinism requirement

If a keyboard byte arrives at reference at step N but at our emulator at step N+1, the two sides would diverge immediately. The comparator would halt on what's really a timing artifact, not a correctness bug. So byte delivery must happen at the same step on both sides.

Reference's keyboard processing happens inside the timer-tick service block (`8086tiny.c:761-762`):

```c
if (int8_asap && !seg_override_en && !rep_override_en && regs8[FLAG_IF] && !regs8[FLAG_TF])
    pc_interrupt(0xA), int8_asap = 0, SDL_KEYBOARD_DRIVER;
```

`SDL_KEYBOARD_DRIVER` expands to `KEYBOARD_DRIVER` on Linux (no SDL), which expands to:

```c
read(0, mem + 0x4A6, 1) && (int8_asap = (mem[0x4A6] == 0x1B), pc_interrupt(7))
```

The `read(0, ...)` is redirected to `harness_read(0, ...)` via EMU-25's source patching. Currently `harness_read` for fd=0 returns 0 unconditionally.

Our emulator's keyboard processing happens in `run.c:597-611`:

```c
if (s->int8_asap && (s->flags & FLAG_IF) &&
    s->seg_override_en == 0 && s->rep_override_en == 0) {
    pc_interrupt(s, 0xA);
    s->int8_asap = 0;

    /* Poll keyboard */
    if (p->console_in.buf) {
        uint8_t key;
        if (ringbuf_read(&p->console_in, &key) == 0) {
            s->mem[0x4A6] = key;
            if (key == 0x1B) s->int8_asap = 1;
            pc_interrupt(s, 7);
        }
    }
}
```

Both sides: (a) enter keyboard-check only at timer-tick moments, (b) write any byte to mem[0x4A6], (c) set int8_asap=1 if byte is ESC (0x1B), (d) fire pc_interrupt(7). Symmetric. Good.

So the synchronisation strategy is: at each timer tick (which fires at the same step on both sides), both emulators query the harness for the next keyboard byte. If one is available, both receive the same byte; if not, both receive "nothing." Either way, lockstep holds.

### What this task does

Add a keyboard input pipe `/tmp/emu86-harness/kbd.in`. The harness reads from it into a small byte buffer. When the timer-tick firing step happens, the harness delivers one byte to each emulator:

- **Reference side**: `harness_read(0, buf, 1)` is updated to return a byte from the harness's buffer (or 0 if buffer is empty). The existing reference code handles "read returned 1 byte" correctly.
- **Our side**: the harness writes the byte to `our_platform.console_in` ringbuf *just before* our emulator runs the step where the timer-tick fires. Our existing ringbuf_read code handles the rest.

Delivery is "peek, then consume after both sides observe." That is: when reference's harness_read is called at step N, the harness returns a byte from the head of the buffer without removing it. The byte is also pushed into our side's ringbuf. When our side's step N completes, the byte is consumed from the buffer (so the next byte is available for step N+1's timer tick, if one fires).

Wait — actually that's more complex than needed. Let me simplify:

**Simpler model**: both sides read keyboard at the same step. If we can guarantee both sides call "get next byte" at the same step, we can just dequeue once per step (if demanded). The harness maintains a queue; at each step, if the timer tick is firing on both sides (which it does when aligned), the harness hands one byte to each side. Since the reference code does `read(0, ...)` and our code does `ringbuf_read(...)` in the same step, both need a byte available at that step.

Cleanest implementation:

- Harness maintains a byte queue (`uint8_t kbd_queue[N]`, head/tail pointers).
- Background thread (or select-based polling) reads from the kbd.in fifo, appending to the queue.
- On the reference side, `harness_read(0, buf, 1)` checks the queue: if non-empty, copies one byte to buf, advances a "peek head" pointer but doesn't remove. Returns 1. Else returns 0.
- On our side, the harness writes one byte into our emulator's console_in ringbuf *before* each step (right after peek on reference side).
- At end of step, the harness advances the queue's real head pointer past the peeked byte.

Actually, let me reconsider. The issue is that the two emulators run sequentially in the harness (reference runs, then we run, then compare). So the ordering is:

1. Reference runs step N. During this, it might call harness_read and get byte B.
2. Harness checks if reference consumed a byte this step. If so, it writes byte B to our emulator's console_in ringbuf.
3. We run step N. During this, our emulator might read the byte from the ringbuf.
4. Comparator checks state. If byte was delivered consistently, states match.

This works naturally. Reference runs first, possibly consumes a byte. Harness observes that a byte was consumed, replicates to our side. Our side consumes the same byte (via ringbuf) at the same step.

Concretely:

```c
/* Per-step, around the "step reference then step our emulator" loop */

/* Before reference step: prepare keyboard delivery.
 * The harness_read override will be called by reference during its step
 * and will consume from kbd_queue if the timer-tick condition fires. */
uint32_t kbd_consumed_before = kbd_queue_head;

/* Run reference step (this is inside HARNESS_STEP_END) */
/* ... reference has run; kbd_queue_head may have advanced by 1 */

if (kbd_queue_head > kbd_consumed_before) {
    /* Reference consumed a byte at step N; deliver same byte to our side */
    uint8_t byte = kbd_queue[kbd_consumed_before];  /* the byte that was consumed */
    ringbuf_write(&our_platform.console_in, byte);
}

/* Now run our emulator step */
emu86_run(our_state, &our_platform, ..., 1, &yi);
/* Our emulator's run.c logic will find the byte in console_in and process it */

/* Compare */
```

One byte per step, delivered to both sides atomically. Lockstep preserved.

### What this task does NOT do

- Does not change the emulator source (`src/emulator/`).
- Does not change any existing unit tests.
- Does not add a higher-level TTY abstraction (raw bytes only; ANSI sequences, line editing, etc. are the user's problem — or the writer of the feeding script).
- Does not handle keyboard scan codes, just ASCII bytes. The reference's KEYBOARD_DRIVER writes to mem[0x4A6] as-is.
- Does not require this to work when `HARNESS_SPLIT_OUTPUT=0`. The keyboard input is independent of output mode — works either way.
- Does not change overrides.c's `harness_read` for fd != 0. Disk reads still passthrough to real libc.

## Your task

### Phase 1 — Read the current keyboard input paths

- `harness/overrides.c` — the `harness_read` function (currently returns 0 for fd=0).
- `reference/8086tiny.c:145-147` — the KEYBOARD_DRIVER macro.
- `reference/8086tiny.c:761-762` — where KEYBOARD_DRIVER gets invoked (inside timer-tick service).
- `src/emulator/run.c:597-611` — our emulator's keyboard polling.
- `src/emulator/platform.h` — the console_in ringbuf definition.
- `harness/harness.c` — init code around lines 310-320 (where console_in is set up for our side) and the step-end logic where keyboard processing currently happens (nowhere, effectively — ringbuf is always empty).

Understand where bytes flow on both sides.

### Phase 2 — Design the kbd queue and fifo reader

Add to `harness/harness.c`:

```c
#define KBD_QUEUE_SIZE 256

static uint8_t kbd_queue[KBD_QUEUE_SIZE];
static uint32_t kbd_queue_head = 0;   /* next byte to read */
static uint32_t kbd_queue_tail = 0;   /* next write position */
static int kbd_fifo_fd = -1;
static int kbd_enabled = 0;
```

Function to create and open the fifo (similar to EMU-31's pattern):

```c
static void setup_kbd_input(void)
{
    const char *env = getenv("HARNESS_KBD_INPUT");
    if (!env || strcmp(env, "1") != 0) return;
    kbd_enabled = 1;

    mkdir("/tmp/emu86-harness", 0755);

    const char *kbd_path = "/tmp/emu86-harness/kbd.in";
    if (mkfifo(kbd_path, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "harness: mkfifo(%s) failed: %s\n", kbd_path, strerror(errno));
        exit(1);
    }

    /* O_RDWR | O_NONBLOCK: never blocks, and "write end" is always present
     * so poll/read sees no spurious EOF. */
    kbd_fifo_fd = open(kbd_path, O_RDWR | O_NONBLOCK);
    if (kbd_fifo_fd < 0) {
        fprintf(stderr, "harness: open(%s) failed: %s\n", kbd_path, strerror(errno));
        exit(1);
    }

    fprintf(stderr, "harness: keyboard input active.\n"
                    "         To send bytes: echo -n 'dir\\r' > %s\n", kbd_path);
    fflush(stderr);
}
```

Function to drain available bytes from fifo into queue (called periodically, e.g., once per harness step):

```c
static void drain_kbd_fifo(void)
{
    if (!kbd_enabled) return;
    while (1) {
        uint32_t free_space = (KBD_QUEUE_SIZE - (kbd_queue_tail - kbd_queue_head));
        if (free_space == 0) break;  /* queue full; skip */
        uint8_t buf[64];
        size_t to_read = free_space < sizeof(buf) ? free_space : sizeof(buf);
        ssize_t n = read(kbd_fifo_fd, buf, to_read);
        if (n <= 0) break;  /* no data or error; try again next step */
        for (ssize_t i = 0; i < n; i++) {
            kbd_queue[kbd_queue_tail % KBD_QUEUE_SIZE] = buf[i];
            kbd_queue_tail++;
        }
    }
}
```

### Phase 3 — Hook into harness_read (reference side)

Modify `harness/overrides.c`:

```c
/* Defined in harness.c; exposed as a single function that returns one byte
 * from the keyboard queue, or 0 if empty. Does NOT advance the queue — the
 * caller (harness.c step loop) advances after both sides have observed. */
extern int harness_peek_kbd_byte(uint8_t *byte_out);
extern void harness_consume_kbd_byte(void);

/* read(): forward to real libc read() for any fd except stdin (0). */
ssize_t harness_read(int fd, void *buf, size_t count)
{
    if (fd == 0) {
        /* Keyboard stdin: pull from harness queue. */
        if (count == 0) return 0;
        uint8_t byte;
        if (!harness_peek_kbd_byte(&byte))
            return 0;  /* no key available */
        /* Reference expects the byte at mem+0x4A6; caller handles writing.
         * We return 1 and copy the byte to their buffer. */
        ((uint8_t *)buf)[0] = byte;
        return 1;
    }
    return read(fd, buf, count);
}
```

Wait — there's a subtle issue. The reference's harness_read *consumes* the byte from the queue the moment it returns 1. But we want to *peek*, not consume, because our side also needs to see the byte.

Revised approach: the queue head advances when reference reads. Our side doesn't need to consume from the same queue — it just needs to receive the byte via its ringbuf. The harness ensures this by writing to our console_in ringbuf whenever reference consumes a byte.

So: `harness_read` consumes directly. The harness.c step loop observes "did reference consume a byte this step?" by checking if kbd_queue_head changed, and if so, writes the consumed byte to our ringbuf before our step runs.

Simpler than the peek-then-consume dance. Let me revise Phase 3:

```c
/* harness/overrides.c */
extern int harness_consume_kbd_byte(uint8_t *byte_out);
/* Returns 1 and writes byte_out if a byte was consumed; 0 if queue empty. */

ssize_t harness_read(int fd, void *buf, size_t count)
{
    if (fd == 0) {
        if (count == 0) return 0;
        uint8_t byte;
        if (!harness_consume_kbd_byte(&byte))
            return 0;
        ((uint8_t *)buf)[0] = byte;
        return 1;
    }
    return read(fd, buf, count);
}
```

`harness_consume_kbd_byte` in harness.c:

```c
int harness_consume_kbd_byte(uint8_t *byte_out)
{
    if (kbd_queue_head == kbd_queue_tail) return 0;  /* empty */
    *byte_out = kbd_queue[kbd_queue_head % KBD_QUEUE_SIZE];
    kbd_queue_head++;
    /* Remember that reference consumed a byte this step, so we can
     * replicate it to our side before our step runs. */
    last_consumed_byte = *byte_out;
    last_consumed_byte_present = 1;
    return 1;
}
```

Static in harness.c:

```c
static uint8_t last_consumed_byte = 0;
static int last_consumed_byte_present = 0;
```

### Phase 4 — Hook into our-side delivery (before our step runs)

In harness.c's step loop, just before calling `emu86_run(our_state, ..., 1, ...)`:

```c
if (last_consumed_byte_present) {
    ringbuf_write(&our_platform.console_in, last_consumed_byte);
    last_consumed_byte_present = 0;
}
```

Our emulator's run.c will observe the byte in the ringbuf at its keyboard-polling point and process it normally (write to mem[0x4A6], fire pc_interrupt(7) if needed).

### Phase 5 — Drain fifo once per step

Also in the step loop (before reference runs, or just at the start of each step):

```c
drain_kbd_fifo();
```

This moves any pending bytes from the kbd.in fifo into the queue. Called once per step — so buffering latency is one step (~0.5ms at current speed), negligible for human typing.

### Phase 6 — Initialisation order

`setup_kbd_input()` should be called in harness init, after the scratch-file setup, alongside `maybe_setup_split_output()` from EMU-31. No dependency on split-output mode — keyboard input works regardless of output mode.

### Phase 7 — Verify default behaviour unchanged

Without `HARNESS_KBD_INPUT=1`, `harness_read(0, ...)` should still return 0 (queue empty, drain does nothing). Existing harness runs proceed as before.

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img
```

Should still advance past 1.6M steps with no divergence.

### Phase 8 — Verify keyboard input works

```
HARNESS_KBD_INPUT=1 ./harness/harness reference/bios test/images/freedos.img
```

Expected: harness starts, FreeDOS boots to A:\> prompt. Harness reports "keyboard input active" at startup.

In another terminal, feed some bytes:

```
echo -n $'dir\r' > /tmp/emu86-harness/kbd.in
```

(The `$''` syntax allows `\r` escape; you want CR, not LF, for DOS line termination.)

Expected: FreeDOS sees "dir<CR>", processes it, outputs a directory listing. Harness continues executing without divergence.

Also verify with both output pipes attached (combines EMU-31 and EMU-32):

```
HARNESS_SPLIT_OUTPUT=1 HARNESS_KBD_INPUT=1 ./harness/harness reference/bios test/images/freedos.img
```

Two terminals with `cat` on ref.out and hux.out. A third with `echo -n $'dir\r' > /tmp/emu86-harness/kbd.in`. Expected: both cat windows show the dir command echo and listing, identically. No divergence.

This is the minimum acceptance criterion: typing a DOS command and seeing it processed by both emulators.

### Phase 9 — Verify ESC handling

The KEYBOARD_DRIVER macro sets `int8_asap = 1` if the byte is 0x1B (ESC). Our emulator's run.c:601 does the equivalent:

```c
if (key == 0x1B) s->int8_asap = 1;
```

Both sides do this symmetrically. Send an ESC byte:

```
echo -n $'\e' > /tmp/emu86-harness/kbd.in
```

Expected: no divergence. Both sides set int8_asap=1 on receiving ESC, and the following timer tick fires accordingly.

### Phase 10 — Full test suite

```
make clean
make all
make test
```

Expected: all tests pass. No emulator changes mean no unit test changes.

### Phase 11 — Commit

Commit if:
- Phase 7: default (no flag) behaviour unchanged — harness still advances past 1.6M steps.
- Phase 8: keyboard input flag works — typing "dir\r" produces dir listing on both outputs, no divergence.
- Phase 9: ESC handled symmetrically.
- Phase 10: tests pass.
- `git status` shows:
  - `harness/harness.c` modified
  - `harness/overrides.c` modified
  - `tasks/emu32-task.md` created
  - No emulator changes, no Makefile changes, no test file changes.

Commit message:

```
EMU-32: Keyboard input via named pipe with synchronous delivery

Adds HARNESS_KBD_INPUT=1 mode. Creates /tmp/emu86-harness/kbd.in
fifo. Harness drains bytes from the fifo into a queue each step.
When the reference calls harness_read(0,...) at a timer tick,
the harness returns the queue's head byte and also writes the
same byte into our emulator's console_in ringbuf before our
step runs. Both sides observe the same byte at the same
simulated step, preserving lockstep.

Combined with EMU-31's split output mode, this makes the
harness interactive:
- Two terminals with `cat` on ref.out and hux.out show FreeDOS
  boot and command output in parallel.
- Another terminal with `echo -n $'cmd\r' > kbd.in` feeds
  commands.
- The comparator continues to verify lockstep throughout.

Design notes:
- Byte-level (ASCII), not scancode-level. Reference's
  KEYBOARD_DRIVER writes the byte to mem[0x4A6] and fires
  pc_interrupt(7); our emulator does the same via ringbuf.
- ESC (0x1B) sets int8_asap=1 on both sides (existing behaviour).
- O_RDWR | O_NONBLOCK on the fifo; no blocking, no spurious EOF.
- Drain once per harness step; one-step latency for typing.
- Queue size 256 bytes; plenty for human typing speed.

Scope:
- harness/harness.c: queue, fifo setup, drain, delivery hook.
- harness/overrides.c: harness_read returns bytes from queue for fd=0.

Verification:
- Default: no regression; harness still advances past 1.6M steps.
- Flag on: typing "dir\r" produces dir listing on both output
  pipes, identical, no divergence.
- ESC byte handled symmetrically.
- make test green.

Follow-up:
- EMU-33: disk write sync verification.
- EMU-34: documented acceptance procedure for debug.com → HELLO.COM.
```

Task log entry:

```
## EMU-32
Date: {today}
Status: PASS
Test results: unchanged (harness-only change)
Harness: adds HARNESS_KBD_INPUT=1 mode. Bytes written to
/tmp/emu86-harness/kbd.in are delivered synchronously to
both emulators at the same simulated step. Typing a DOS
command and seeing it processed on both sides verified.
Notes: Combined with EMU-31's split output, the harness
is now an interactive DOS emulator with lockstep verification.
```

Then:

```
mv tasks/emu32-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 12 — Report (on failure)

Triage if:
- Phase 8 shows divergence after typing a byte (synchronisation is off).
- Phase 8's "dir" doesn't reach both sides (delivery is not working).
- Phase 9's ESC handling diverges (symmetry issue).
- Default behaviour regresses (Phase 7 fails).

## Out of scope — do not touch

- Emulator code (src/emulator/).
- Reference source (reference/8086tiny.c).
- Makefile.
- Comparator logic.
- Pre-step snapshot (EMU-28).
- Split-output mode (EMU-31).
- Disk write sync (EMU-33).
- Any test file changes.
- Keyboard scan codes, special keys beyond ASCII+ESC.
- Dormant concerns.

## Final note

Failure modes to watch for:

1. **Byte-consumption race.** `harness_read` consumes from the queue during reference's step execution. Our side's delivery happens *before* our step runs. If the two sides' step ordering is not "reference first, then ours," this breaks. Check harness_step_end carefully to confirm the order. (In the current harness, HARNESS_STEP_END fires after reference's step completes and before our emulator runs — which is the right ordering for this to work.)

2. **Queue wraparound.** kbd_queue_head and kbd_queue_tail are uint32_t, increment unboundedly. Use `% KBD_QUEUE_SIZE` for array access only, not for comparing "queue empty" (head == tail means empty; they just grow forever). If you need to detect overflow (unlikely), `(tail - head) > KBD_QUEUE_SIZE` works — but with human typing into a 256-byte queue drained every ~0.5ms, overflow is practically impossible.

3. **Non-blocking open caveat.** Some Linux versions are picky about opening a fifo O_RDWR vs O_RDONLY | O_NONBLOCK. O_RDWR should Just Work; if you hit issues, try `O_RDWR | O_NONBLOCK`.

4. **Line endings.** DOS uses \r (0x0D) for command entry, not \n (0x0A). `echo -n $'dir\r'` is the right incantation. `echo "dir"` sends "dir\n" which DOS may or may not process correctly. The reference's behaviour here is what matters; we match it. If typing "dir\r" works but "dir\n" causes divergence, that's a hint — but most likely both produce the same behaviour on both sides (one of them might fail to produce a dir listing, but that's a DOS-level issue, not a harness issue).

5. **ESC behaviour.** The 0x1B check sets int8_asap=1 on both sides. This causes an extra timer tick fire next opportunity. Intended behaviour; verify it stays symmetric.

6. **Silent drops.** If the queue is full when drain_kbd_fifo runs, incoming bytes are dropped. For human typing at 256-byte buffer, this never happens. For scripted input of very large blobs, it could. Document this; don't worry about it for now.

Once EMU-32 lands, the harness is interactive. You can type, see output, verify lockstep. Next task (EMU-33) will verify disk writes are also in lockstep, closing the loop on "anything the user does inside the emulator is verified correct."
