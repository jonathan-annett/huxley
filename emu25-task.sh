#!/usr/bin/env bash
# emu25-task.sh — EMU-25: Switch harness override mechanism from preprocessor -D
# substitution to JS-based source patching.
#
# Run with: bash emu25-task.sh
#
# This script is self-contained: it writes the brief to tasks/emu25-task.md and
# then invokes `claude "read ... and follow it exactly"`. The brief content
# lives in a quoted heredoc below so nothing in it gets shell-expanded.
#
# Running it more than once overwrites the brief file (harmless if you haven't
# edited it between invocations).

set -euo pipefail

BRIEF_PATH="tasks/emu25-task.md"

# The brief. Quoted heredoc ('EOF') means no variable/backtick expansion.
cat > "$BRIEF_PATH" <<'EOF'
# EMU-25: Switch harness overrides from -D preprocessor substitution to JS source patching

**You are a fresh Claude Code session.** You are not continuing a previous task. Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

The differential test harness (`packages/emu86/harness/`) builds the reference emulator `reference/8086tiny.c` with four preprocessor substitutions intended to redirect non-deterministic libc calls to harness-controlled replacements:

```
-Dread=harness_read
-Dtime=harness_time
-Dftime=harness_ftime
-Dlocaltime=harness_localtime
```

**These substitutions have been non-functional since they were added.** We discovered this today while investigating what looked like a new harness divergence at step 65,771. After instrumenting the harness to dump state at the divergence boundary and running with stdin redirected to `/dev/null`, we established:

- Without `< /dev/null`: ref takes an int7 (keyboard) interrupt at step 65,771 that ours doesn't, diverging.
- With `< /dev/null`: no divergence at step 65,771. Harness advances cleanly to step 66,392, where a real divergence (FLAGS/ZF on ADD SI, AX) is waiting.

The cause is that `-Dread=harness_read` doesn't actually redirect `read()` calls in the compiled binary. Verified via objdump:

```
$ objdump -d harness/harness | grep -cE 'call.*<harness_read'
0
$ objdump -d harness/harness | grep -cE 'call.*<read@plt'
4
```

Zero calls to `harness_read`; four calls to real `read@plt`. Every `read(...)` in the reference, including the `read(0, mem+0x4A6, 1)` in the `KEYBOARD_DRIVER` macro, goes to real libc. The stdin one reads whatever's buffered in the terminal, which contaminates test runs non-deterministically.

The most likely cause of the substitution failure is glibc's fortify-source machinery in `<unistd.h>`, which uses `__asm__("read")` aliases to bind the symbol name at link time independently of the preprocessor macro name. The `-D` substitution renames the macro call but the linker still resolves to real `read`.

### What this task does

Replaces the `-D` substitution mechanism with an explicit source-patching step, done in JavaScript (Node), that rewrites `read(...)` → `harness_read(...)` etc. in a patched copy of the reference source. The original `reference/8086tiny.c` is never modified; the patched copy lands in `harness/obj/reference_patched.c` and is the file actually compiled.

Why JS rather than sed:
- More readable and easier to maintain than sed syntax.
- Can print a summary to stderr of how many occurrences each patch modified, which surfaces substitution failures at build time rather than at runtime.
- Extensible: future harness-related source adaptations can be added to the same script.

### What this task does NOT do

- Not the step-66,392 ZF divergence investigation. That's EMU-26. This task is purely about getting the harness override mechanism working.
- Not fixing the existing stdin leakage issue structurally (we could, in principle, also close stdin in harness `main()` as defence in depth — but the source-patching fix is sufficient and more general).
- Not touching `src/emulator/` at all. Harness infrastructure only.

## Your task

### Phase 1 — Confirm the current broken state

Run the objdump check against the existing harness binary:

```
cd packages/emu86
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<harness_read"
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<read@plt"
```

Expected: 0 and 4 (or similar). If `harness_read` count is nonzero, the override is somehow already working and this brief's premise is wrong — stop and report.

Also verify the current stdin-contamination behaviour:

```
# With stdin connected to terminal — may diverge at step 65,771
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -5

# With stdin redirected — diverges at step 66,392 instead
./harness/harness reference/bios test/images/freedos.img < /dev/null 2>&1 | tail -5
```

You should see different divergence steps depending on stdin state. This is the contamination EMU-25 fixes.

### Phase 2 — Create the patch script

Create `packages/emu86/harness/patch-reference.js` with this content (adjust if you see a cleaner shape):

```javascript
#!/usr/bin/env node
/*
 * patch-reference.js — transforms reference/8086tiny.c for use in the
 * differential harness.
 *
 * Applies source-level substitutions that the preprocessor can't
 * reliably handle (glibc fortify-source defeats -Dread=harness_read
 * etc at the linker level). Reference source is never modified; this
 * script writes to stdout and the Makefile captures it to
 * harness/obj/reference_patched.c.
 *
 * Usage:  node patch-reference.js <input.c>  > output.c
 */

const fs = require('fs');

const input = fs.readFileSync(process.argv[2], 'utf8');

const patches = [
    {
        name: 'redirect read(...) to harness_read',
        from: /\bread\s*\(/g,
        to: 'harness_read(',
    },
    {
        name: 'redirect time(...) to harness_time',
        from: /\btime\s*\(/g,
        to: 'harness_time(',
    },
    {
        name: 'redirect ftime(...) to harness_ftime',
        from: /\bftime\s*\(/g,
        to: 'harness_ftime(',
    },
    {
        name: 'redirect localtime(...) to harness_localtime',
        from: /\blocaltime\s*\(/g,
        to: 'harness_localtime(',
    },
];

let output = input;
let totalReplaced = 0;
for (const p of patches) {
    const count = (output.match(p.from) || []).length;
    totalReplaced += count;
    output = output.replace(p.from, p.to);
    console.error(`patch-reference: ${count} occurrence${count === 1 ? '' : 's'} — ${p.name}`);
}
console.error(`patch-reference: total ${totalReplaced} substitutions`);

if (totalReplaced === 0) {
    console.error('patch-reference: WARNING — no substitutions applied; patches may be out of date');
    process.exit(1);
}

process.stdout.write(output);
```

`chmod +x` is not required — we'll invoke as `node patch-reference.js`.

**Verification of the script in isolation:**

```
cd packages/emu86
node harness/patch-reference.js reference/8086tiny.c > /tmp/patched-preview.c
diff reference/8086tiny.c /tmp/patched-preview.c | head -30
```

Expected: a diff showing `read(...)` → `harness_read(...)`, `time(...)` → `harness_time(...)`, etc. Nothing unexpected should change.

Sanity-check that the patched file still compiles standalone (quick smoke test — won't link but should at least preprocess/parse):

```
gcc -DNO_GRAPHICS -DHARNESS_STEP_BEGIN=harness_step_begin \
    -DHARNESS_STEP_END=harness_step_end -Dmain=reference_main \
    -c /tmp/patched-preview.c -o /tmp/patched-test.o 2>&1 | head -20
```

Expected: compiles cleanly (perhaps with some warnings, but no errors). If it fails, the patch script is too aggressive or too weak — review the diff and narrow the regex accordingly.

### Phase 3 — Update the Makefile

Find the harness section of `packages/emu86/Makefile` (around `HARNESS_REF_CFLAGS`). Make these changes:

**Remove the four `-D` substitutions** from `HARNESS_REF_CFLAGS`:

```
# Before:
HARNESS_REF_CFLAGS = -O2 -Wall -DNO_GRAPHICS \
    -DHARNESS_STEP_BEGIN=harness_step_begin \
    -DHARNESS_STEP_END=harness_step_end \
    -Dmain=reference_main \
    -Dread=harness_read \
    -Dtime=harness_time \
    -Dftime=harness_ftime \
    -Dlocaltime=harness_localtime

# After:
HARNESS_REF_CFLAGS = -O2 -Wall -DNO_GRAPHICS \
    -DHARNESS_STEP_BEGIN=harness_step_begin \
    -DHARNESS_STEP_END=harness_step_end \
    -Dmain=reference_main
```

**Add a rule that generates the patched source and compiles it:**

```
$(HARNESS_OBJ_DIR)/reference_patched.c: reference/8086tiny.c harness/patch-reference.js | $(HARNESS_OBJ_DIR)
	node harness/patch-reference.js $< > $@

$(HARNESS_OBJ_DIR)/reference.o: $(HARNESS_OBJ_DIR)/reference_patched.c | $(HARNESS_OBJ_DIR)
	$(CC) $(HARNESS_REF_CFLAGS) -c $< -o $@
```

Note: the `reference.o` rule's *dependency* changes from `reference/8086tiny.c` directly to `$(HARNESS_OBJ_DIR)/reference_patched.c`. The patched file is generated from the pristine source plus the patch script, so Make correctly rebuilds when either changes.

**Update the clean rule** if it mentions the patched file (or just leave it; the whole `$(HARNESS_OBJ_DIR)` is rm-rf'd on clean anyway).

### Phase 4 — Rebuild and verify

```
cd packages/emu86
make clean
make harness 2>&1 | tail -20
```

Expected output: the `node harness/patch-reference.js` invocation runs, prints the "N occurrences" summary to stderr (visible in the build log), then gcc compiles the patched file. No errors.

Then verify the substitutions actually took effect in the binary:

```
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<harness_read"   # expect > 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<read@plt"       # expect 0 (or 0 for fd-0 calls)
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<harness_time"   # expect > 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<time@plt"       # expect 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<harness_ftime"  # expect > 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<ftime@plt"      # expect 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<harness_localtime"  # expect > 0
objdump -d harness/harness 2>/dev/null | grep -cE "call.*<localtime@plt"      # expect 0
```

Note about `read@plt`: the `harness_read` wrapper itself calls real `read` for non-stdin fds (disk passthrough). That call is inside `overrides.c` which is compiled *without* the patch — so `read@plt` may still appear once, inside `harness_read`. That's fine and expected. What should NOT appear is calls to `read@plt` from within `reference.o`. If you want to be precise:

```
objdump -d harness/harness --disassemble=harness_read 2>/dev/null | grep -E "call.*<read"
# expected to show the real read call inside harness_read — that's fine
```

### Phase 5 — Behavioural verification

The acceptance test: the harness behaves the same with and without `< /dev/null`. Before EMU-25, the two runs gave different results. After EMU-25, they must match.

```
cd packages/emu86
# Run 1 — normal invocation
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -10 > /tmp/run-normal.txt

# Run 2 — stdin closed
./harness/harness reference/bios test/images/freedos.img < /dev/null 2>&1 | tail -10 > /tmp/run-nostdin.txt

# Compare
diff /tmp/run-normal.txt /tmp/run-nostdin.txt
```

Expected: zero difference (apart from the `tail: ...file truncated` line from reopening `/tmp/emu86-harness/heartbeat.log`, which is tail(1)'s own output and unrelated).

Both runs should show the harness reaching step 66,392 before diverging on FLAGS/ZF at `ADD SI, AX`. That's the next real divergence, and it's EMU-26, not this task's problem.

**If the runs differ:** something is still non-deterministic. Stop and report. Could be a time-related call the patch missed, or a different stdin-reading code path, or something else.

### Phase 6 — Standalone check

`./emu86` (the non-harness binary) is a separate target and isn't affected by these changes, but confirm it still builds and runs:

```
make emu86
./emu86 reference/bios test/images/freedos.img
```

Expected: reaches FreeDOS kernel banner as before. This regression guard is cheap to run.

### Phase 7 — Commit

**Commit** if:
- Phase 1 confirmed the broken baseline.
- Phase 2's script compiles cleanly and produces a sensible diff.
- Phase 3's Makefile changes build cleanly.
- Phase 4's objdump check shows substitutions now working.
- Phase 5's behavioural comparison shows identical output in both runs.
- Phase 6's standalone check is green.

Commit message:

```
EMU-25: Fix harness override mechanism via JS source patching

Bug: The harness's -Dread=harness_read (and -Dtime, -Dftime,
     -Dlocaltime) preprocessor substitutions have been non-functional
     since they were added. glibc fortify-source machinery in
     <unistd.h> uses __asm__("read") aliases that bind the symbol
     name at link time, bypassing the -D rewrite. objdump confirmed
     zero calls to harness_read, four to real read@plt.

Impact: Every harness run was reading real stdin via the reference's
        KEYBOARD_DRIVER macro. Terminal state contaminated test
        runs. The step-65,771 divergence that looked new was
        actually this contamination — with stdin redirected to
        /dev/null, the harness advances cleanly to step 66,392
        (the real next divergence, to be addressed in EMU-26).

Fix: Replace -D substitutions with a Node script that patches
     reference/8086tiny.c before compilation. The original source
     is never modified; the patched version lands in
     harness/obj/reference_patched.c and is what gets compiled.

Changes:
- harness/patch-reference.js: new, applies substitutions for
  read/time/ftime/localtime with summary output to stderr.
- Makefile: reference.o now builds from the patched file, and
  the -D substitutions are removed from HARNESS_REF_CFLAGS.
- No emulator-side changes.

Verification:
- objdump shows calls to harness_read/time/ftime/localtime in
  harness binary; real {read,time,ftime,localtime}@plt calls
  only inside the harness_* wrappers in overrides.c (expected).
- Harness run with and without < /dev/null now produces identical
  output.
- Standalone ./emu86 behaviour unchanged.

Follow-up:
- EMU-26: investigate step-66,392 FLAGS/ZF divergence on
  ADD SI, AX at 1FE0:7C9D (or whichever CS:IP precedes it).
```

Task log entry:

```
## EMU-25
Date: {today}
Status: PASS
Test results: unchanged — this is harness-infrastructure only
Harness: now reaches step 66,392 reliably regardless of stdin state;
         previously diverged at step 65,771 when stdin was connected
         to a terminal with input buffered.
Notes: Replaced failing -D preprocessor substitutions with JS
source patching. harness/patch-reference.js applies read/time/
ftime/localtime redirections. Makefile builds reference.o from
the patched copy; pristine reference/8086tiny.c untouched.
objdump confirms substitutions now effective in the binary.
```

Then:

```
mv tasks/emu25-task.md tasks/completed/
git add -A
git commit
```

Do not push. User reviews.

### Phase 8 — Report (on failure)

Report to `tasks/triage/emu25-triage-report.md` if:
- Phase 1 shows `harness_read` was already being called (premise falsified).
- Phase 2's script produces unexpected diff output or compile failures.
- Phase 3's Makefile change breaks build.
- Phase 4's objdump still shows zero harness_read calls after rebuild.
- Phase 5's runs diverge in behaviour between stdin states.
- Phase 6's standalone regresses.

## Out of scope — do not touch

- **The step-66,392 ZF divergence.** EMU-26 territory.
- **Structurally closing stdin in harness main() as defence-in-depth.** Nice-to-have but separate task.
- **Any emulator-side changes.**
- **Any test file changes** unless Makefile quirks require it (unlikely).
- **Any non-harness Makefile targets** (the `emu86` target, the `test-*` targets, etc.).
- **The Makefile-header-dependency quirk** flagged during EMU-20. Separate issue.
- Previous out-of-scope items: editor-api-proposal, latent 0xEA length bug, 0xC0/0xC1 rotate-form reference bug, silent-exit-on-0:0, register-memory aliasing, timer design questions, FreeDOS divide-by-zero, exec_lea dead code (already resolved).

## Housekeeping

- Scratch files in `/tmp/emu86-harness/` per EMU-17. Heartbeat log lives alongside them.
- Heartbeat instrumentation and EMU-21 opcode-bytes printer in harness.c must be retained.
- No new binaries in the commit. Verify `git status` before `git add`.
- Pristine `reference/8086tiny.c` must remain untouched. If `git status` shows it modified, something went wrong — stop and report.

## Final note

This is the first task where we're explicitly modifying how the reference source gets processed before compilation. Worth being careful about:

1. **The patched file is a generated artifact.** Like `reference.o`, it should NOT be committed. If `harness/obj/reference_patched.c` shows up in `git status` and gets staged, revert it. Put `harness/obj/` in `.gitignore` if it isn't already. (Actually it likely is — `reference.o` doesn't appear in commits either.)

2. **The regex patterns are simple and might miss edge cases.** Run `diff reference/8086tiny.c $(HARNESS_OBJ_DIR)/reference_patched.c | wc -l` during verification — the number should be small and all the changes should be reasonable. If the diff is enormous or includes unexpected regions, the regex is too aggressive.

3. **If future libc-override needs emerge**, they go into `patch-reference.js` as new patch entries. Don't add new `-D` substitutions to the Makefile — they won't work, for the same reason the original four didn't.

4. **This task is infrastructure, not forward progress.** No harness advance until EMU-26. The payoff is that future harness runs are reliable regardless of terminal state — which matters a lot for the eventual editor-project integration, where the harness might run in contexts where stdin is unpredictable.

If anything unexpected surfaces during Phase 4 or Phase 5 verification, stop and report rather than improvising.
EOF

echo "Brief written to: $BRIEF_PATH"
echo "Invoking Claude Code..."
echo ""

claude "read $BRIEF_PATH and follow it exactly"
