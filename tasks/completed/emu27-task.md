# EMU-27: Stop clearing AF after logical operations (AND, OR, XOR, TEST)

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

With EMU-26 landed, the harness now advances past step 66,392 (the MUL/ZF divergence) and halts at step 69,489 on an AF mismatch at CS:IP=1FE0:7D20.

The divergent instruction (at pre-step CS:IP=1FE0:7D1D, bytes `80 E4 0F`) is:

`AND AH, 0x0F`

This is a logical operation. Intel documents that AND/OR/XOR/TEST set:
- CF = 0
- OF = 0
- SF, ZF, PF: from result
- **AF: undefined**

Our `set_flags_logic` helper explicitly clears AF anyway, with the comment "AF is undefined per Intel docs but we clear it for consistency." 8086tiny does not touch AF in the logical-op path — it leaves AF at whatever value preceded the instruction. The two approaches are both Intel-legal but produce different observable state, and the harness catches it.

### Evidence

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -20
```

Expected: divergence at step 69,489 on `AND AH, 0x0F` at CS:IP=1FE0:7D20, with only AF differing between ref and ours.

The pattern is structurally identical to EMU-20 (AF after SHL/SHR/SAR). Both cases: Intel-undefined flag, reference quietly leaves it alone, we explicitly clear it and cause drift.

### Relevant reference code

`reference/8086tiny.c:705-706` — the post-dispatch flags update for logical ops:

```c
if (set_flags_type & FLAGS_UPDATE_OC_LOGIC)
    set_CF(0), set_OF(0);
```

Zeros CF and OF. Does NOT touch AF.

### Relevant emulator code

`src/emulator/opcodes/helpers.h:140-152` — `set_flags_logic`:

```c
/*
 * Set flags for logic operations (AND, OR, XOR, TEST).
 * CF and OF are always cleared. SF, ZF, PF set normally.
 * AF is undefined per Intel docs but we clear it for consistency.
 */
static inline void __attribute__((always_inline))
set_flags_logic(Emu86State *s, uint32_t result, uint8_t width)
{
    set_flags_szp(s, result, width);
    clear_flag(s, FLAG_CF);
    clear_flag(s, FLAG_OF);
    /* AF is undefined for logic ops; clear it */
    clear_flag(s, FLAG_AF);
}
```

The two AF-related lines — the comment and the `clear_flag` — are the only changes needed.

### What this task does

Remove the AF-clear from `set_flags_logic`. Update the function's doc comment to reflect that AF is now left untouched, matching the reference. Also update per-opcode docstrings in `logic.h` where they say "AF=0" for the logical ops — those comments are now wrong.

Add tests that verify AF is preserved across AND/OR/XOR/TEST operations. Following EMU-20's pattern: set AF=1 before the operation, verify AF=1 after; set AF=0 before, verify AF=0 after. That pins the new behaviour and prevents future regression.

### What this task does NOT do

- Does not touch AF behaviour for ADD/SUB/ADC/SBB/INC/DEC/CMP/NEG — those all have Intel-defined AF semantics and our code already handles them correctly (per EMU-20's review).
- Does not touch AF behaviour for MUL/IMUL — EMU-26 established that AF is left untouched for those; that's already correct.
- Does not touch AF behaviour for shifts — EMU-20 already aligned those.
- No `run.c` changes.
- No Makefile changes.
- No harness changes.

## Your task

### Phase 1 — Confirm the divergence

```
cd packages/emu86
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -25
```

Expected output includes:

```
Step: 69489
Ref CS:IP: 1FE0:7D20
Our CS:IP: 1FE0:7D20
Categories: 0x00000008 (FLAGS only)
FLAGS: ref=... ours=...  xor=<something with bit 4 set>
  AF: ref=<val> ours=<!val>
```

Bit 4 is AF in the FLAGS packing. If the xor has other bits set, this brief's scope may be wrong — stop and report.

### Phase 2 — Identify all AF-clearing sites for logical ops

Grep for the AF clear in the logical-op path:

```
grep -n "FLAG_AF" src/emulator/opcodes/helpers.h src/emulator/opcodes/logic.h
grep -rn "AF=0" src/emulator/opcodes/logic.h
```

Expected: one `clear_flag(s, FLAG_AF)` in `set_flags_logic` (helpers.h:~151), and comments in logic.h that say "AF=0" for AND/OR/XOR/TEST. Both should be addressed. If there are additional AF-clear sites in the logical-op path that I didn't anticipate, list them in the commit message.

### Phase 3 — Make the code change

In `src/emulator/opcodes/helpers.h`:

- Remove the `clear_flag(s, FLAG_AF)` line from `set_flags_logic`.
- Update the doc comment to reflect the new behaviour. Suggested wording:

```c
/*
 * Set flags for logic operations (AND, OR, XOR, TEST).
 * CF and OF are cleared. SF, ZF, PF set from result.
 * AF is Intel-undefined for logic ops; we leave it untouched to
 * match 8086tiny reference semantics (see EMU-27).
 */
```

In `src/emulator/opcodes/logic.h`:

- For each of AND/OR/XOR/TEST, update the doc comment that says "AF=0" to something like "AF unchanged (Intel-undefined)".
- The `NOT` comment already correctly says "NO flags affected" — no change.

### Phase 4 — Unit tests

Locate the existing logic-op tests (grep for `exec_and` or `test_logic` in `test/unit/`). Add tests that explicitly verify AF preservation.

Test pattern, repeated for AND / OR / XOR / TEST:

1. Set AF=1 in state, run the op with operands that produce a specific result. Verify:
   - SF/ZF/PF: correct from result
   - CF=0, OF=0
   - **AF=1** (unchanged)

2. Set AF=0 in state, run the same op. Verify:
   - AF=0 (unchanged)

That's 8 new test points (4 ops × 2 pre-states).

Follow the existing test style. If there are existing logic-op tests that implicitly assume "AF=0 after", those tests will now fail. Update them to reflect the new behaviour (their post-AF-expectation should be "whatever was set before the op"). Note any such updates in the commit message.

**Also extend the AF regression guard:** add a test where one of the logical ops is preceded by something that sets AF=1 (e.g. an ADD with appropriate operands), then the logical op runs, then verify AF is still 1. That's the exact shape of the bug the harness caught.

### Phase 5 — Verify

```
cd packages/emu86
make test-logic
./test/unit/test_logic
```

Expected: all tests pass.

Then:

```
make emu86
./emu86 reference/bios test/images/freedos.img
```

Expected: reaches FreeDOS banner. (Divide-by-zero at banner is a known separate issue.)

Then harness:

```
touch src/emulator/run.c && make harness
./harness/harness reference/bios test/images/freedos.img 2>&1 | tail -15
```

**Expected outcome:** harness advances PAST step 69,489. Report the next divergence step, CS:IP, and category. That's EMU-28.

### Phase 6 — Full test suite

```
make test
```

Expected: all tests pass.

### Phase 7 — Commit

Commit if:
- Phase 1 confirmed AF-only divergence at step 69,489.
- Phase 3 changes are minimal (helpers.h and logic.h only).
- Phase 4 tests all pass, including the AF regression guard.
- Phase 5 harness advances past step 69,489.
- Phase 6 full test suite is green.
- `git status` shows only:
  - `src/emulator/opcodes/helpers.h` modified
  - `src/emulator/opcodes/logic.h` modified (comments only, most likely)
  - `test/unit/test_logic.c` modified (new AF tests; possibly existing tests updated)
  - `tasks/emu27-task.md` created
  - No other emulator or harness files changed
  - No binaries
  - No changes to `reference/`, `bios.asm`, or EMU-25's patch script

Commit message:

```
EMU-27: Stop clearing AF after logical operations

Intel documents AF as undefined after AND, OR, XOR, TEST. 8086tiny
(our reference oracle) leaves AF at its pre-op value. Our
set_flags_logic helper was explicitly clearing AF with the rationale
"we clear it for consistency" — which is a reasonable choice in
isolation but produces visible drift from the reference. The
differential harness caught it at step 69,489 on an AND AH, 0x0F
instruction at CS:IP=1FE0:7D20.

Structurally identical to EMU-20 (AF-after-shifts): Intel-undefined
flag, reference leaves untouched, we were explicitly clearing.
Alignment is the correct choice.

Scope:
- set_flags_logic in helpers.h: remove clear_flag(s, FLAG_AF)
- Doc comments in helpers.h and logic.h updated to reflect that
  AF is Intel-undefined and left untouched
- New tests in test_logic.c verify AF preservation across
  AND/OR/XOR/TEST for both AF=0 and AF=1 pre-states
- Regression guard: ADD-that-sets-AF followed by AND; verify
  AF survives

Verification:
- test-logic passes including new AF tests and regression guard
- Full make test green
- Standalone ./emu86 reaches FreeDOS banner
- Harness advances past step 69,489 (next divergence at step {N}
  is EMU-28)

Follow-up:
- EMU-28: whatever the harness finds after 69,489
```

Task log entry:

```
## EMU-27
Date: {today}
Status: PASS
Test results: N → N+8+ (AF preservation tests plus regression guard)
Harness: advances past step 69,489; next divergence at step {N}
Notes: Removed explicit AF clear from set_flags_logic. Another
Intel-undefined case, same shape as EMU-20 (shifts), EMU-26 (MUL).
The pattern is clear: when Intel says "undefined", the reference's
answer is typically "leave alone" — our job is to match that even
though "clear it" is equally Intel-legal.
```

Then:

```
mv tasks/emu27-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 8 — Report (on failure)

Triage to `tasks/triage/emu27-triage-report.md` if:
- Phase 1 shows divergence categories other than FLAGS, or flag bits other than AF differing (premise is partially wrong).
- Phase 3 changes are non-trivial beyond the two expected files.
- Phase 5 harness regresses or fails to advance.
- Phase 6 shows unrelated test failures.
- Existing logic-op tests fail in ways that reveal a design assumption elsewhere in the codebase about "AF is zero after logic ops" that this brief didn't anticipate.

## Out of scope — do not touch

- AF behaviour for any instruction class other than AND/OR/XOR/TEST.
- Any run.c changes.
- Any Makefile changes (including the header-dependency quirk).
- Harness infrastructure.
- The dormant items: editor-api-proposal, 0xEA length bug, 0xC0/0xC1 reference bug, silent-exit-on-0:0, register-memory aliasing, FreeDOS divide-by-zero.
- EMU-25 artifacts.

## Final note

Two failure modes to watch for:

1. **The "AF=0 after logic op" assumption might be baked into other tests.** If you find yourself updating multiple existing tests to reflect the new AF behaviour, pause and audit why they existed. Each one is a place where the old behaviour was explicitly validated. The audit isn't to preserve them (they were testing the wrong thing) but to make sure you're not missing a case where "AF=0 after logic" was actually load-bearing — e.g. if an IVT-related instruction depended on it. Unlikely, but worth a minute's thought.

2. **Don't generalise the fix beyond what's asked.** It might be tempting to also audit other "undefined flag" helpers and align them proactively. Don't. That's its own task. EMU-20 handled shifts, EMU-26 handled MUL/IMUL SZP, this handles logical-op AF. Future Intel-undefined divergences will surface via the harness and get addressed in their own tasks. The pattern is reactive by design; premature alignment without harness evidence risks changing behaviour in ways that produce NEW divergences.

The recurring shape across EMU-20/26/27 is now clear enough to note in docs eventually — but that's a documentation task, not this one. Just flag it in the commit message if you notice it naturally.
