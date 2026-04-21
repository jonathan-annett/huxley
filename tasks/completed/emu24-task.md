# EMU-24: Harness heartbeat log — visibility into long runs and hangs

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

This monorepo (`lite`, codename `hux`) is building a clean-room refactored 8086 emulator called `emu86` at `packages/emu86/`. Task-based workflow: one task per commit, completed tasks moved to `tasks/completed/`, task log at `tasks/completed/task-log.md`.

### Where we are

The differential test harness (EMU-16/17/19/20/22/23) runs our emulator in lockstep with the reference `8086tiny`. It has advanced steadily through FreeDOS boot and is now reaching step counts around 66,000+ where the run appears to hang rather than diverge or complete — probably because FreeDOS has reached a point where it's polling for keyboard input, and neither emulator has any input to feed it.

The harness currently has limited observability into long runs. A progress line prints to stderr every 100,000 steps. Between those lines, the harness is silent — if it's stuck somewhere, we can't tell where.

This task adds a lightweight heartbeat log so we can see what the harness is doing mid-run, diagnose hangs, and generally observe progress.

### What this task is *not*

- Not keyboard input. That's a separate, larger task (EMU-25 or later).
- Not per-emulator console output tagging. Also separate.
- Not keeping the harness running past a hang. If it's stuck, we still need to Ctrl-C. The heartbeat gives us a way to see *where* it got stuck, not to get past it.
- Not a fix for any known divergence. After this lands, the harness will still do whatever it's currently doing (hang around step 66k, or whatever it turns out to be doing).

## Your task

### Phase 1 — Understand the current state

Skim the harness's main step hook in `packages/emu86/harness/harness.c` (the `HARNESS_STEP_END` body, around line 630-680). Notice:

- The existing progress print every 100,000 steps (roughly `if (harness_step_count % 100000 == 0) ...`).
- The step-limit-exit block with the opcode-bytes instrumentation added during EMU-21's preparation.
- The scratch directory creation (`mkdir(HARNESS_SCRATCH_DIR, ...)` in `main()`).

The heartbeat work is additive — it doesn't replace or change any of this.

### Phase 2 — Design decisions

Settled before writing code:

**Log file location:** `/tmp/emu86-harness/heartbeat.log`. Same directory as the scratch files, already owned by the harness.

**Append or truncate:** Truncate on harness startup. Each run produces a fresh log. No accumulation across runs.

**Frequency:** Every 1,000 steps by default. Overridable via env var `HARNESS_HEARTBEAT_EVERY`:

- `HARNESS_HEARTBEAT_EVERY=100` for deep-zoom debugging.
- `HARNESS_HEARTBEAT_EVERY=0` to disable heartbeats entirely (if any future use case wants that).
- Unset or invalid → default 1,000.

**Log line format:**

```
step=NNNNNN cs=XXXX ip=XXXX dt_ms=MM opcodes=XX XX XX XX XX XX XX XX
```

Fields:

- `step` — current instruction count, zero-padded or not (your call; legibility wins).
- `cs`, `ip` — 4-hex CS and IP from the reference. (They match ours at heartbeat time since a divergence would have halted first.)
- `dt_ms` — wall-clock milliseconds since the last heartbeat, rounded to integer. On the first heartbeat, dt_ms is wall-clock since harness initialisation.
- `opcodes` — 8 bytes at CS:IP, space-separated hex. Same format as the step-limit-exit instrumentation. Helps identify what instruction is about to execute, which makes hang diagnosis concrete ("polling at 0x1234:5678 running 74 FE" = tight JZ loop).

Example:

```
step=001000 cs=F000 ip=10F8 dt_ms=2 opcodes=D2 EF 88 EB 2E 80 3E 72
step=002000 cs=F000 ip=102A dt_ms=1 opcodes=EB 08 90 90 90 90 90 90
step=003000 cs=1FE0 ip=7C62 dt_ms=1 opcodes=8D 66 A0 FB 80 7E 24 FF
```

**Flush discipline:** Every line must be flushed immediately so `tail -f` shows it in real time. A fire-and-forget `fprintf` without flush would buffer and make the log useless for hang diagnosis.

### Phase 3 — Implement

Edit `packages/emu86/harness/harness.c`.

**Add module-level state near the top, with the other harness globals (~line 108):**

```c
static FILE     *harness_heartbeat_fp;
static uint64_t  harness_heartbeat_every = 1000;
static uint64_t  harness_last_heartbeat_us;
```

(Use whatever naming convention matches the existing file.)

**In `main()`, after the scratch directory is created (~line 788-792), initialise the heartbeat log:**

1. Parse `HARNESS_HEARTBEAT_EVERY` from env. If set and parses as a non-negative integer, use it. If `0`, disable heartbeats. If unset or invalid, keep default 1000.
2. If heartbeats enabled, open `/tmp/emu86-harness/heartbeat.log` for writing (truncate). If open fails, print a warning to stderr but continue (don't halt the harness just because the log can't be written).
3. Initialise `harness_last_heartbeat_us` to current wall-clock time in microseconds.

**In the step-end hook (`HARNESS_STEP_END` body), after `harness_step_count++` and after the compare-and-divergence block, but before the step-limit check, add the heartbeat:**

```c
if (harness_heartbeat_fp &&
    harness_heartbeat_every > 0 &&
    harness_step_count % harness_heartbeat_every == 0) {
    harness_emit_heartbeat();
}
```

Where `harness_emit_heartbeat()` is a small helper:

1. Compute current wall-clock time in microseconds.
2. Compute `dt_ms = (now_us - harness_last_heartbeat_us) / 1000`.
3. Build the opcode-bytes string by reading 8 bytes from `mem[CS*16 + IP]` (same pattern as the step-limit-exit code).
4. `fprintf` the line in the format above.
5. `fflush(harness_heartbeat_fp)`.
6. Update `harness_last_heartbeat_us = now_us`.

For wall-clock microseconds, use `clock_gettime(CLOCK_MONOTONIC, ...)` — it's already a POSIX.1 dependency you're likely using. Compute `tv_sec * 1000000 + tv_nsec / 1000`.

**At harness exit paths** (the step-limit-exit `exit(0)` and the divergence `exit(2)` in the step-end hook, and the `exit(1)` paths in `main()`), **close the heartbeat log if open** so any buffered data is flushed. You can do this by registering an `atexit()` handler that closes `harness_heartbeat_fp`, which is the cleanest approach and handles all exit paths uniformly.

### Phase 4 — Verify

Run the harness and check that heartbeats appear. Several checks:

**Test 1 — basic operation:**

```
cd packages/emu86
make clean && make harness
# Run until the step-65,770 era or the hang, whichever comes first
./harness/harness reference/bios test/images/freedos.img &
sleep 2
head /tmp/emu86-harness/heartbeat.log
# (and to confirm it's growing)
wc -l /tmp/emu86-harness/heartbeat.log
sleep 3
wc -l /tmp/emu86-harness/heartbeat.log  # should be higher
# terminate the run
kill %1
```

Expected: heartbeat file exists, contains lines in the format specified, grows while the harness runs.

**Test 2 — truncation on startup:**

Append a sentinel line manually to `/tmp/emu86-harness/heartbeat.log`, then start a new harness run. Confirm the sentinel is gone from the new log.

**Test 3 — env var override:**

```
HARNESS_HEARTBEAT_EVERY=100 ./harness/harness reference/bios test/images/freedos.img &
sleep 1
wc -l /tmp/emu86-harness/heartbeat.log
kill %1
```

Expected: line count is ~10x what the default run produced in the same duration.

**Test 4 — disabled heartbeats:**

```
HARNESS_HEARTBEAT_EVERY=0 ./harness/harness reference/bios test/images/freedos.img &
sleep 2
ls -l /tmp/emu86-harness/heartbeat.log 2>&1  # should either not exist, or be empty
kill %1
```

Expected: no heartbeats written (file doesn't exist, or exists empty from the truncate).

**Test 5 — hang diagnosis (the actual use case):**

Run with the default step limit (200M or whatever the harness's default is). Watch the heartbeat log with `tail -f` in another terminal. Note the last step at which heartbeats appear before they stop (meaning the harness is hung). Kill the run. Verify the heartbeat log's last line matches the last observed step and points to a sensible CS:IP.

**Unit tests:** This is harness infrastructure, not emulator logic. No unit tests required unless you see a natural place to add one. The Phase 4 integration checks are the acceptance tests.

**Harness regression:** Confirm the harness still:

- Reports divergences correctly (use `-e HARNESS_INJECT_DIVERGENCE=1 HARNESS_INJECT_AT=100` or whatever the existing injection hooks are, if needed — check what EMU-16 established).
- Exits cleanly at step limit.
- Runs to the current known pattern (hang around step 66k, or natural exit when step limit reached earlier).

**Standalone emulator:** Unchanged. You're not touching `src/emulator/` at all.

### Phase 5 — Commit

Commit message:

```
EMU-24: Harness heartbeat log for long-run visibility

The differential harness has been running past step 66,000 where
FreeDOS appears to be polling for keyboard input, producing a hang
that was previously hard to diagnose — the existing 100k-step
progress line wasn't fine-grained enough to tell where the hang
began.

Adds a heartbeat log at /tmp/emu86-harness/heartbeat.log.

Format (one line per heartbeat):
  step=NNNNNN cs=XXXX ip=XXXX dt_ms=MM opcodes=XX XX XX XX XX XX XX XX

- Step count and wall-clock interval tell us whether the harness is
  advancing or stuck.
- CS:IP plus opcode bytes show what instruction is about to execute,
  which makes tight-polling-loop diagnosis concrete.

Cadence default: every 1000 steps. Override via HARNESS_HEARTBEAT_EVERY
env var (0 disables). Log is truncated on each startup.

Verified: heartbeats appear during normal runs; env var override and
disable both work; harness still reports divergences and exits
cleanly at step limit. Standalone emu86 unchanged (harness-only
change).

Follow-up:
- EMU-25: investigate the apparent hang around step 66k using the
  new heartbeat log to pinpoint where the tight loop is.
- Future: named pipes per emulator for console output visibility,
  scripted keyboard input for unblocking DOS prompts.
```

Task log entry:

```
## EMU-24
Date: {today}
Status: PASS
Test results: {X passed, Y failed} — no new unit tests (harness infrastructure)
Harness: still runs; heartbeats now visible in /tmp/emu86-harness/heartbeat.log
Notes: Added heartbeat log to harness.c. Default 1000-step cadence,
env-var override, truncate-on-startup, real-time flush. Sets up
EMU-25 investigation of the apparent hang around step 66k.
```

Then:

```bash
mv tasks/emu24-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

### Phase 6 — Report (on failure)

Report to `tasks/triage/emu24-triage-report.md` if any of Phase 4's tests fail in ways you can't quickly resolve, or if the heartbeat implementation requires structural changes to harness.c that the brief didn't anticipate.

## Out of scope — do not touch

- **Keyboard input support.** Even though this task was motivated by an apparent keyboard-polling hang, the heartbeat doesn't try to solve the hang — it just makes it diagnosable. Keyboard input is a separate task.
- **Per-emulator console output tagging.** Also separate.
- **Named pipes for display output.** Mentioned in the follow-up as a future direction; not this task.
- **Any emulator-side changes.** This task is purely harness instrumentation.
- **The step-66k divergence / hang itself.** EMU-25.
- Previous out-of-scope items: editor-api-proposal, latent 0xEA length bug, 0xC0/0xC1 rotate-form reference bug, silent-exit-on-0:0, register-memory aliasing, timer design questions, FreeDOS divide-by-zero.

## Housekeeping

- Scratch files in `/tmp/emu86-harness/` per EMU-17. The heartbeat log lives alongside them.
- Opcode-bytes-at-step-limit instrumentation from EMU-21 stays. (You'll notice the heartbeat reuses the same opcode-reading pattern — that's fine, don't refactor into a shared helper unless it's trivial.)
- No new binaries in the commit. Verify `git status` before `git add`.
- Makefile quirk: `touch src/emulator/run.c && make harness` if builds seem stale. (Probably not an issue for this task since we're only touching `harness.c`.)

## Final note

This is harness infrastructure work, not emulator work. The bar for "done" is that the heartbeat log does what the brief describes, and the harness's existing behaviour is unchanged for everything else. No subtlety, no investigation, no alignment judgements.

Three specific things to get right:

1. **Real-time flush.** A buffered heartbeat is useless for hang diagnosis. Every line must be flushed immediately. If you see anything that looks like the heartbeat is lagging behind the harness's actual progress during testing, fix the flush.

2. **Truncate on startup.** If heartbeats accumulate across runs, the log becomes unmanageable and the "last line shows where it hung" diagnostic stops working.

3. **Don't break the existing harness.** The heartbeat additions must be purely additive. If anything about the harness's divergence detection, step-limit handling, or self-test behaviour changes, something's wrong — stop and re-check.

If anything unexpected happens during Phase 4 verification, stop and report rather than improvising.
