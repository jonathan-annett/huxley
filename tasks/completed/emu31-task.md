# EMU-31: Split harness stdout into two named pipes for parallel observation

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

Post-EMU-30, the emulator reaches the FreeDOS `A:\>` prompt and the harness verifies lockstep execution through at least 1.69M instructions with no divergence. We've crossed from "building the emulator" to "using the emulator."

The next phase of work needs the emulator to be *interactive*: a keyboard input path, visible screen output, and eventually full DOS usage (running `debug.com` to write small programs, for example). Before adding keyboard input, we need observable output — so when we start typing, we can see the echo on both sides and confirm lockstep holds through interactive use.

Currently the harness handles console output asymmetrically:
- **Reference**: writes directly to fd 1 via `write(1, regs8, 1)` inside `8086tiny.c:677`. Output appears in the harness's stdout (terminal or pipe).
- **Our emulator**: writes into a `console_out` ringbuf. The harness silently drains the ringbuf at each step (`harness.c:702-703`) and discards the bytes. Reason: if we also emitted, we'd double-print everything on the terminal.

This asymmetry works for lockstep verification (divergence in AL at a PUTCHAR step would be caught by the register comparator), but it's opaque. We can see what the reference produces but not what our emulator produces. For interactive use we need both visible, side-by-side.

### What this task does

Add an optional mode to the harness where reference output and our output each go to a separate named FIFO (pipe):

- `/tmp/emu86-harness/ref.out` — reference's fd 1 output.
- `/tmp/emu86-harness/hux.out` — our emulator's `console_out` ring buffer drain.

Enabled by an environment variable `HARNESS_SPLIT_OUTPUT=1` (default off, so existing invocations behave as before).

When enabled, the user can open two terminal windows, each running `cat /tmp/emu86-harness/ref.out` or `cat /tmp/emu86-harness/hux.out`, and watch the FreeDOS boot sequence render character-by-character in both panes. If the two panes look identical, output lockstep is visually confirmed.

### What this task does NOT do

- Does not add keyboard input (that's EMU-32).
- Does not change default behaviour. Without `HARNESS_SPLIT_OUTPUT=1`, the harness behaves exactly as today.
- Does not touch emulator source (`src/emulator/`). All changes are in the harness.
- Does not touch divergence reporting, comparator logic, EMU-28 snapshots, or any correctness machinery.
- Does not modify the reference's 8086tiny.c. The redirection is done at fd-open time in the harness.

### Design notes

**Why named pipes and not plain files.** Pipes block the writer when no reader is attached, which is perfect for our case: the harness pauses naturally if you don't have `cat` attached, and resumes streaming when you do. Also, there's no unbounded file growth.

**Opening fifos in non-blocking mode on the writing side.** If we just `open(fifo, O_WRONLY)` without a reader, we block forever. Two options:
- Open `O_RDWR` (always works; a "self-reader" trick).
- Open non-blocking (`O_WRONLY | O_NONBLOCK`), handle EPIPE/EAGAIN gracefully.

`O_RDWR` is simpler. The harness never reads back from the fifo; it just uses the fd to write. The kernel treats `O_RDWR` as "there's a reader" for blocking purposes. Use that.

**FIFO creation.** `mkfifo(path, 0666)`. If the fifo already exists from a previous run, `mkfifo` returns EEXIST; that's fine — just open the existing one. Don't unlink at startup (another cat reader might have it open from a prior session).

**Reference's fd 1.** The reference writes via `write(1, ...)`. Redirecting means `dup2(fifo_fd, 1)` early in harness initialisation, before the reference runs any PUTCHAR. The agent should determine the right place to do this — somewhere after argument parsing but before the main run loop starts.

**Our side's output.** The harness already drains `console_out` ringbuf at harness.c:702-703. When split-output mode is on, instead of discarding, write each drained byte to the hux fifo's fd.

**When split-output is disabled.** Don't create fifos at all. Don't touch fd 1. Existing behaviour preserved.

### Acceptance criteria

Running the harness with the flag should produce identical character-by-character output in two terminals attached via cat. Specifically, the FreeDOS boot sequence (kernel version, copyright, driver loads, `A:\>` prompt) must appear on both.

**No lockstep check is needed beyond the existing comparator.** The comparator already catches divergences in register state, which would include AL at PUTCHAR time. If the two outputs ever visibly differ character-by-character, the existing harness would have halted with a divergence. The split-output mode is purely an observation tool, not a new correctness check.

## Your task

### Phase 1 — Read the existing output paths

- `harness/harness.c` around lines 697-703 (the `console_out` drain).
- `harness/harness.c` at init (around lines 160-330): look for where our_platform is set up.
- `reference/8086tiny.c:677` — reference's `write(1, regs8, 1)`.
- `harness/harness.c` near the `main()` area: the reference's stdin/stdout setup. Find where `reference_main` is invoked (post EMU-17 scratch file setup).

Goal: understand the execution flow enough to know where to insert the fd redirection.

### Phase 2 — Add FIFO setup

At harness initialisation (before `reference_main` runs):

```c
#include <sys/stat.h>
#include <fcntl.h>

static int harness_split_output = 0;
static int ref_fifo_fd = -1;
static int hux_fifo_fd = -1;

static void maybe_setup_split_output(void)
{
    const char *env = getenv("HARNESS_SPLIT_OUTPUT");
    if (!env || strcmp(env, "1") != 0) return;
    harness_split_output = 1;

    mkdir("/tmp/emu86-harness", 0755);  /* already made by scratch-file code, but harmless */

    const char *ref_path = "/tmp/emu86-harness/ref.out";
    const char *hux_path = "/tmp/emu86-harness/hux.out";

    if (mkfifo(ref_path, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "harness: mkfifo(%s) failed: %s\n", ref_path, strerror(errno));
        exit(1);
    }
    if (mkfifo(hux_path, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "harness: mkfifo(%s) failed: %s\n", hux_path, strerror(errno));
        exit(1);
    }

    ref_fifo_fd = open(ref_path, O_RDWR);
    if (ref_fifo_fd < 0) {
        fprintf(stderr, "harness: open(%s) failed: %s\n", ref_path, strerror(errno));
        exit(1);
    }
    hux_fifo_fd = open(hux_path, O_RDWR);
    if (hux_fifo_fd < 0) {
        fprintf(stderr, "harness: open(%s) failed: %s\n", hux_path, strerror(errno));
        exit(1);
    }

    /* Redirect reference's fd 1 to the ref fifo. */
    if (dup2(ref_fifo_fd, 1) < 0) {
        fprintf(stderr, "harness: dup2 failed: %s\n", strerror(errno));
        exit(1);
    }

    fprintf(stderr, "harness: split-output mode active.\n"
                    "         Open a terminal and: cat %s\n"
                    "         Open another and:    cat %s\n",
                    ref_path, hux_path);
    fflush(stderr);
}
```

Call `maybe_setup_split_output()` at an appropriate point in harness init — before `reference_main` is invoked, but after the scratch-file setup (so the "harness: scratch files" banner still goes to real stderr).

### Phase 3 — Forward our side's console_out

Modify the drain loop at `harness.c` around lines 702-703:

```c
/* Drain console_out: either discard (default) or forward to hux fifo. */
uint8_t ch;
while (ringbuf_read(&our_platform.console_out, &ch) == 0) {
    if (harness_split_output) {
        ssize_t w = write(hux_fifo_fd, &ch, 1);
        (void)w;  /* best effort; don't halt on partial write or EPIPE */
    }
    /* else: discard silently, as before */
}
```

No other changes to the drain loop or surrounding code.

### Phase 4 — Verify mode-off behaviour unchanged

Without `HARNESS_SPLIT_OUTPUT=1`, nothing should change. The reference writes to fd 1 (terminal), our side's ringbuf drains silently.

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img
```

Expected: behaviour identical to pre-EMU-31 (FreeDOS banner appears on terminal, harness runs until step limit or divergence).

### Phase 5 — Verify mode-on behaviour

Open three terminals.

**Terminal A** (reader for ref):
```
cat /tmp/emu86-harness/ref.out
```

**Terminal B** (reader for hux):
```
cat /tmp/emu86-harness/hux.out
```

(Note: the fifos may not exist yet. That's OK — `cat` on a non-existent fifo will fail. Either pre-run the harness once to create them, or use `while true; do cat /tmp/emu86-harness/ref.out 2>/dev/null || sleep 1; done` as a reconnect loop. Or just run the harness first, then open the cats in other terminals; since the harness opens the fifos with O_RDWR, it won't block waiting for readers.)

**Terminal C** (harness):
```
cd packages/emu86
HARNESS_SPLIT_OUTPUT=1 ./harness/harness reference/bios test/images/freedos.img
```

Expected:
- Terminal A shows the FreeDOS boot sequence (version banner, copyright, prompt).
- Terminal B shows character-by-character identical content.
- Terminal C shows the harness init messages ("split-output mode active..."), heartbeats, and any divergence or step-limit message — but NOT the FreeDOS banner, because that went to the fifo, not stdout.

**If the two terminals (A and B) show identical content**, split-output works.

**If they differ character-by-character at some point**, that's interesting — it would imply our emulator is producing output the reference isn't (or vice versa). The comparator would have caught this as a register divergence at the PUTCHAR step, but if split-output reveals a difference the comparator missed, that's a new category of bug to investigate. Unlikely given the comparator's thoroughness, but possible. If observed, report as a finding.

### Phase 6 — Full build and test

```
cd packages/emu86
make clean
make harness
make test
```

Expected: all tests pass. No changes should be needed to any test — this is harness-only.

### Phase 7 — Commit

Commit if:
- Phase 4: default behaviour is unchanged.
- Phase 5: split-output mode shows identical output on both fifos (visual confirmation).
- Phase 6: all tests pass.
- `git status` shows:
  - `harness/harness.c` modified
  - `tasks/emu31-task.md` created
  - No emulator changes, no Makefile changes, no test file changes
  - No binaries

Commit message:

```
EMU-31: Split harness output into two named pipes for observability

Post-EMU-30 the emulator reaches the FreeDOS A:\> prompt and the
harness runs clean past 1.6M instructions. Moving toward interactive
use (keyboard input, running debug.com, etc.) requires observable
output on both sides in parallel.

Adds an HARNESS_SPLIT_OUTPUT=1 environment flag that redirects:
- reference fd 1 (stdout) to /tmp/emu86-harness/ref.out
- our emulator's console_out ringbuf to /tmp/emu86-harness/hux.out

Both are named pipes. Open two terminals, cat each pipe, watch the
FreeDOS boot render character-by-character in both. Visual
confirmation of output lockstep.

Default behaviour (no flag set) is unchanged — reference writes
to terminal, our side's ringbuf drains silently, just as before.

Scope:
- harness/harness.c: add split-output setup, forward drain to
  hux fifo when flag is set.
- No emulator changes.
- No new tests (this is observation infrastructure; comparator
  still catches divergence in register state at PUTCHAR steps).

Verification:
- Default: FreeDOS banner still on terminal as before.
- Flag on: two cat processes on the fifos show identical
  character-by-character output through full FreeDOS boot.
- Harness advances to the same step count under both modes.
- make test passes.

Follow-up:
- EMU-32: keyboard input plumbing (harness reads stdin, feeds
  both emulators synchronously).
- EMU-33: disk-write sync verification (both scratch images
  should be identical at end of run).
- EMU-34: documented acceptance procedure for running debug.com
  inside the harness to produce HELLO.COM.
```

Task log entry:

```
## EMU-31
Date: {today}
Status: PASS
Test results: unchanged (harness-only change)
Harness: adds HARNESS_SPLIT_OUTPUT=1 mode. Reference fd 1 and
our console_out each go to a named pipe. Verified visually:
FreeDOS boot renders identically in both fifos.
Notes: Step toward interactive harness use. Default behaviour
preserved. No emulator correctness changes.
```

Then:

```
mv tasks/emu31-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 8 — Report (on failure)

Triage to `tasks/triage/emu31-triage-report.md` if:
- Split-output mode shows divergent output between the two fifos (new class of bug).
- Default behaviour breaks (something unexpected about the dup2 or init ordering).
- Full build fails.
- Test suite regresses.

## Out of scope — do not touch

- Emulator source (`src/emulator/`).
- Reference source (`reference/8086tiny.c`).
- Makefile.
- Comparator logic.
- Pre-step snapshot (EMU-28).
- Keyboard input (EMU-32).
- Disk write sync (EMU-33).
- Dormant concerns.

## Final note

Failure modes to watch for:

1. **Fifo blocking if opened write-only without a reader.** `open(fifo, O_WRONLY)` blocks until a reader attaches. Use `O_RDWR` instead — the harness never reads from the fifo, but `O_RDWR` makes the kernel treat it as "reader present," so the write-side never blocks.

2. **init ordering for fd 1 redirection.** The `dup2(ref_fifo_fd, 1)` must happen *before* the reference begins executing. Otherwise early PUTCHAR calls go to the terminal instead of the fifo. The scratch-file-setup output currently goes to stderr, so it isn't affected by the redirection — which is good.

3. **Previous fifo files on disk.** If a prior run (or a crashed run) left fifos in `/tmp/emu86-harness/`, `mkfifo` returns EEXIST. Treat that as fine — just open the existing fifo. Don't unlink/recreate at startup, because another `cat` reader from a prior session might still have the old fifo open.

4. **Readers disconnecting mid-run.** If the user Ctrl-Cs the `cat` on ref.out, the kernel sends SIGPIPE on the next write. Default is to terminate. Add `signal(SIGPIPE, SIG_IGN)` in harness init if split-output is enabled, so the harness survives a reader disconnect. Writes will return EPIPE after; handle gracefully (drop the byte, don't halt).

This is the first task that gives the harness UI characteristics beyond logging. Design it to be calm and robust — long-running sessions where users connect and disconnect fifo readers shouldn't kill the harness. That robustness matters more as interactive features land in EMU-32 and beyond.
