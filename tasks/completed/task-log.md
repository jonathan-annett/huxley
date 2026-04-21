# EMU86 Task Log

## EMU-01
Date: 2026-03-22
Status: PASS
Test results: N/A (analysis and reference build task)
Notes: Cloned 8086tiny, copied reference source (8086tiny.c, bios.asm, bios binary). Produced comprehensive ORIGINAL-ANALYSIS.md. Reference build compiles with -DNO_GRAPHICS. FreeDOS floppy image included.

## EMU-02
Date: 2026-03-22
Status: PASS
Test results: 390 passed, 0 failed (298 snapshot + 92 ringbuf)
Notes: Created Emu86State, DecodeContext, Emu86Platform, Emu86Tables structs. Field-by-field snapshot serialisation with CRC32. Ring buffer with power-of-2 wrapping.

## EMU-03
Date: 2026-03-22
Status: PASS
Test results: 460 passed, 0 failed (298 snapshot + 92 ringbuf + 70 decode)
Notes: Full instruction decoder with ModRM decode, EA calculation using BIOS tables, and register/memory access helpers. All functions static inline with always_inline on hot paths. Tests load real BIOS binary for table validation. One test expectation adjusted: MOV reg,imm (0xB8) raw decode has i_w=0 since operand width override happens at execution time, not decode time.

## EMU-04
Date: 2026-03-22
Status: PASS
Test results: 618 passed, 0 failed (298 snapshot + 92 ringbuf + 70 decode + 158 arithmetic)
Notes: Full arithmetic opcode family: ADD, ADC, SUB, SBB, CMP, NEG, INC, DEC, MUL, IMUL, DIV, IDIV, plus BCD (DAA, DAS, AAA, AAS, AAM, AAD). Flag helpers in helpers.h with set_flags_add/sub/inc/dec/logic/szp. One task spec correction: IMUL byte (-2)*3=-6 fits in signed byte so CF=OF=0, not 1 as spec stated. Added additional IMUL test case (-128*2=-256) that correctly triggers CF=OF=1.

## EMU-05
Date: 2026-03-22
Status: PASS
Test results: 658 passed, 0 failed (previous 618 + 40 logic)
Notes: Logic opcodes: AND, OR, XOR, NOT, TEST. Used existing set_flags_logic from helpers.h. NOT correctly preserves all flags. read_dest/write_dest/read_src helpers guarded against double-definition with arithmetic.h.

## EMU-06
Date: 2026-03-22
Status: PASS
Test results: 725 passed, 0 failed (previous 658 + 67 shift)
Notes: Shift/rotate opcodes: SHL, SHR, SAR, ROL, ROR, RCL, RCR. Shifts set SF/ZF/PF; rotates do not. RCL/RCR use iterative loop for correct carry-through rotation. Count of 0 is a complete no-op. SAR fills with sign bit. Dispatcher exec_shift_rotate selects by extra field.

## EMU-07
Date: 2026-03-22
Status: PASS
Test results: 775 passed, 0 failed (previous 725 + 50 transfer)
Notes: Data transfer opcodes: MOV variants, PUSH/POP, XCHG, LEA, LDS, LES, CBW, CWD, XLAT, LAHF, SAHF, PUSHF, POPF. Stack helpers (stack_push/stack_pop) designed for reuse by CALL/RET/INT. FLAGS pack/unpack with 8086 reserved bits. SAHF only affects low 8 bits of FLAGS. LEA extracts offset by subtracting segment base from linear address.

## EMU-08
Date: 2026-03-22
Status: PASS
Test results: 856 passed, 0 failed (previous 775 + 81 string)
Notes: String opcodes: MOVSB/W, CMPSB/W, STOSB/W, LODSB/W, SCASB/W. REP/REPZ/REPNZ loop handling with CX countdown. Segment override applies to source (DS:SI) only; destination always ES:DI. Direction flag controls increment/decrement.

## EMU-09
Date: 2026-03-22
Status: PASS
Test results: 941 passed, 0 failed (previous 856 + 85 control)
Notes: Control flow opcodes: JMP (5 variants), Jcc (all 16 conditions), CALL (4 variants), RET/RETF (with/without imm16), INT/INT3/INTO/IRET, LOOP/LOOPZ/LOOPNZ/JCXZ. Added ip_changed field to DecodeContext. pc_interrupt helper for INT and hardware interrupt delivery. All relative jumps are relative to next instruction IP.

## EMU-10
Date: 2026-03-22
Status: PASS
Test results: 1016 passed, 0 failed (previous 941 + 75 flags_io)
Notes: Flag manipulation (CLC/STC/CMC/CLI/STI/CLD/STD), I/O ports (IN/OUT 8 variants), HLT, SALC, prefix handlers (segment override, REP), and BIOS calls (PUTCHAR via ring buffer, GET_RTC, DISK_READ/WRITE via platform callbacks). io_port_write_hook stub for future device emulation.

## EMU-11
Date: 2026-03-23
Status: PASS
Test results: 1316 passed, 0 failed (previous 1016 + 300 run)
Notes: Complete run loop with emu86_run() and emu86_step_single(). Unity build in run.c includes all opcode headers. Full opcode dispatch switch covering all xlat cases 0-48 plus 53 (HLT/LOCK). Key fixes during implementation: HLT maps to xlat=53 (not in original 0-48 range), segment override extra values use original numbering (8-11, converted to 0-3), case 14 JMP/CALL needs manual inst_length since table base_size=0, INT cases need explicit inst_length for correct return address. Tests include: basic execution, arithmetic, conditional jumps, loops, stack, CALL/RET, software interrupts, REP string ops, Fibonacci program, and memory fill.

## EMU-12
Date: 2026-03-23
Status: PASS (unit tests) — manual boot test pending
Test results: 2876 unit passed, 0 failed (previous 1316 + 1560 platform)
Boot tests: FreeDOS: PENDING, ELKs: PENDING (needs interactive terminal)
Notes: Linux host with CLI entry point, terminal raw mode (termios), disk I/O via file descriptors, timer via clock_gettime, console I/O via ring buffers. atexit(terminal_restore) for safety. Snapshot save/load via CLI flags. signal(SIGINT) for clean exit. Init sequence matches original: BIOS at F0100, tables loaded, DL for boot drive, CX:AX for HD size. Platform tests cover ring buffer alloc, disk read/write, timer, snapshot file round-trip. Semi-automated boot test script included.

## EMU-12
Date: 2026-03-23 (implemented), 2026-04-21 (triaged)
Status: PARTIAL — committed, boot blocked pending opcode fix
Test results: 1560 unit passed, 0 failed. Boot: FreeDOS BLOCKED, ELKs BLOCKED (downstream of same bug).
Notes: Linux host implementation complete — CLI entry, termios raw mode, disk I/O, timer, snapshot I/O, signal handling, all unit tested. Triage identified and fixed PUSH/POP sreg dispatch bug (cases 25/26 in run.c mis-indexed sregs[]). Second bug in 0x80–0x83 ALU-imm length calculation identified, not fixed — separate task. See tasks/triage/emu12-triage-report.md for full investigation, including latent concerns flagged for future attention.

## EMU-13
Date: 2026-04-21
Status: PASS
Test results: 329 passed, 0 failed in test_run (up from 303) — 8 new tests covering all four 0x80-0x83 opcodes with register-direct and memory modrm variants (26 new assertions). Full suite still green.
Boot test: FreeDOS fails with a new symptom — the fix unblocks reach_stack_stc, execution advances from 209 instructions (pre-fix, broken IRET) to 156,246 instructions (post-fix) before ending at CS:IP=0:0. This matches the triage's prediction for outcome (b): register-memory mapping gap at 0xF0000 or some other latent issue now surfaces deeper in the boot path.
Notes: Fixed ALU-imm instruction length overcount for opcodes 0x80-0x83 in run.c case 8. Root cause: the decoder's linear length formula counts `iw_size * (operand_width + 1)` bytes for the immediate, then case 8 added 1 or 2 more bytes on top (instead of replacing). Fix: subtract the decoder's overcount before adding the real immediate size — `(sign_ext ? 1 : 2) - (d->operand_width + 1)`. For 0x80/0x81/0x82 the delta is 0 (real size matches decoder guess); for 0x83 the delta is -1 (decoder counted a 2-byte immediate but 0x83's is sign-extended from 1 byte). Added 8 regression tests in test_run.c covering all four opcodes with mod=3 (register-direct) and memory modrm variants (mod=0 rm=6 direct, mod=1 disp8, mod=2 disp16). Verified tests fail against unpatched code (revert-and-re-test) — all 8 flip from HALTED to BUDGET with wrong IP.

## EMU-15
Date: 2026-04-21
Status: PASS
Test results: 342 passed, 0 failed in test_run (up from 329) — 4 new tests for 0xC0/0xC1 length covering register-direct SHL/SHR/ROL and a memory-operand direct-address form (13 new assertions). Full suite still green (2918 total).
Boot test: outcome (b) — FreeDOS now relocates the boot sector, loads the kernel from disk, prints the FreeDOS kernel banner ("FreeDOS kernel - SVN (build 2040 OEM:0xfd) [compiled Apr  7 2012]" and copyright/GPL notice), then fails with "Interrupt divide by zero" — the same downstream failure EMU-14's Phase 2 independent verification predicted. That divide-by-zero becomes the next task.
Notes: Fixed 0xC0/0xC1 instruction-length overshoot in run.c case 12 (removed spurious `d->inst_length++` in the `if (d->extra)` branch). Same bug pattern as EMU-13's case 8 fix: BIOS tables give base_size=3 for 0xC0/0xC1 (already counting opcode+modrm+imm8), so the decoder's linear formula yields the correct length; the `++` over-advanced IP by one byte. Reference 8086tiny balances this by calling `set_opcode(0x10)` (base=2) before the IP advance; our refactor dropped that remap but kept the `++`. Audit of two cases flagged by EMU-14 triage: case 6 sub 0 (TEST r/m, imm for 0xF6/0xF7) is **correct** — base_size=2 and iw_size=0 mean the decoder does not pre-count the immediate, so the case's `+= operand_width + 1` is the right adjustment; case 14 (JMP/CALL for 0xE8-0xEB) is **broken-latent** — the `3 - direction` formula gives 2 for 0xEA (really 5 bytes), but exec_jmp_far sets CS:IP absolutely with ip_changed=1 and never consumes inst_length, so the wrong value is harmless in practice. Added 4 regression tests in test_run.c covering register-direct SHL/SHR/ROL and one memory-operand direct-address form. Verified tests fail against unpatched code (revert-and-re-test) — all 4 flip from HALTED to BUDGET with IP past the HLT.

## EMU-16
Date: 2026-04-21
Status: PASS
Test results: harness self-tests both pass (500-step snapshot-compare selftest reports zero divergence; injected AX=0xBEEF divergence detected at step 1 with correct diagnostic). Full emu86 unit suite still green (2918 total).
Harness run: first real divergence at **step 4096**, CS:IP=F000:02D5 (opcode 81 FA B8 03). Only `int8_asap` differs — ref=0, ours=1. Root cause is a pre-existing bug in `src/emulator/run.c:587`: the timer-tick cadence check is `(s->inst_count & 0x4FFF) == 0`, which fires whenever the low 12 bits of inst_count are zero AND bit 14 is zero (i.e. at 0x1000, 0x2000, 0x3000, 0x8000, 0x9000, 0xA000, 0xB000, ...) rather than every 20000 instructions as the adjacent comment claims. That becomes the next task. 4095 steps cleanly match between the two emulators prior to this.
Notes: Macro-based integration of `reference/8086tiny.c` via `HARNESS_STEP_BEGIN()` / `HARNESS_STEP_END()` hooks with empty defaults (10-line reference diff; standalone reference build produces an md5-identical binary). Non-determinism neutralised by `-Dread=harness_read`, `-Dtime=harness_time`, `-Dftime=harness_ftime`, `-Dlocaltime=harness_localtime` which redirect the reference's libc calls to deterministic stubs (no keyboard input, all-zero RTC). Our side's `platform.get_time_us` returns 0 for the same reason. Harness clones the reference's post-init state into our `Emu86State`, then on every sim() iteration steps our emulator by one instruction (via `emu86_run` with budget=1) and compares: registers, segment registers, IP, flags, prefix counters, trap_flag, int8_asap, spkr_en, io_hi_lo, inst_count, the full 1MB memory (minus the reference's register-mapped region 0xF0000–0xF00FF), and 64KB of I/O ports. Harness shares the reference's disk fds to avoid stateful divergence from parallel seeks.

## EMU-17
Date: 2026-04-21
Status: PASS
Notes: Added scratch-file disk isolation to the harness. Each emulator now operates on its own copy of the disk image; source remains untouched. Self-test confirms image md5 unchanged before/after run. Harness output includes paths to scratch files for offline diff inspection. Phase 5 reproduced EMU-16's divergence finding at step 4096, confirming no regression in CPU-state comparison path.

## EMU-19
Date: 2026-04-21
Status: PASS
Test results: 1565 passed, 0 failed — 5 new tests for timer cadence (first tick at 20000, no tick at 4096, exact-multiple counts, partial-window counts, interval regularity). Revert-and-re-test confirmed all five fail against unpatched code.
Harness result: step-4096 divergence closed; harness now reaches step 65679 before hitting a new divergence — FLAGS AF bit differs at CS:IP=F000:10FA executing `88 EB` (MOV BL, CH). Becomes the next task.
Notes: Fixed timer tick cadence in run.c:587 to match reference's `inst_counter % KEYBOARD_TIMER_UPDATE_DELAY` (=20000). Changed `(s->inst_count & 0x4FFF) == 0` to `(s->inst_count % 20000) == 0` — one-line fix as prescribed. Old bitmask fired at 0x1000, 0x2000, 0x3000, 0x4000, 0x8000, … (irregular); new modulo fires at 20000, 40000, 60000, … (exact cadence). Closed the step-4096 divergence identified by EMU-16 and diagnosed by EMU-18. Standalone `./emu86 reference/bios test/images/freedos.img` unchanged — still reaches FreeDOS kernel banner and divide-by-zero loop.

## EMU-20
Date: 2026-04-21
Status: PASS
Test results: 77 passed, 0 failed in test_shift (up from 67) — 10 new tests for shift/rotate AF. Full unit suite: 2995 total, 0 failed. Revert-and-re-test confirmed the three SHL/SHR/SAR `preserves_af_set` tests fail against unpatched code; rotate-preserves tests pass both before and after (regression guards, as expected).
Harness result: step-65679 divergence closed; harness now reaches step 65770 before hitting a new divergence — prefix-state category, `seg_override_en` differs (ref=1 ours=0), `seg_override` ref=12 ours=1 at CS:IP=1FE0:7C65 on opcode bytes `FB 80 7E 24 FF 75 03 88` (STI, CMP byte ptr [BP+24], 0xFF, …). Becomes the next task.
Notes: Aligned SHL/SHR/SAR with reference by removing `clear_flag(s, FLAG_AF)` from `exec_shl`, `exec_shr`, `exec_sar` in opcodes/shift.h — three deletions. Intel's 8086 spec marks AF undefined after shifts (both clearing and preserving are defensible); we match the reference for harness lockstep. Reference preserves AF by design: `std_flags[0xD2] = 0` (post-dispatch flag block skipped) and the shift dispatch re-maps via `set_opcode(0x10)` to an SZP-only flag entry. Added 6 preserves-AF regression tests for SHL/SHR/SAR (AF=1 and AF=0 starting states) plus 4 rotate preserves-AF guards (ROL/ROR/RCL/RCR with AF=1) so a future refactor can't silently re-introduce AF-clearing into rotates. Quirks doc updated. Standalone `./emu86 reference/bios test/images/freedos.img` unchanged — FreeDOS kernel banner reached, divide-by-zero loop as before.

## EMU-22
Date: 2026-04-21
Status: PASS
Test results: 1563 passed, 0 failed in full unit suite — 3 new runtime-path LEA tests in test_run.c (`run_lea_sets_seg_override_en`, `run_lea_result_unchanged`, `run_lea_flags_unchanged`). Revert-and-re-test confirmed `run_lea_sets_seg_override_en` fails against unpatched run.c (0 != 1); the two regression guards pass both before and after (by design — they protect against fix-induced regressions in LEA's result and flag-neutrality).
Harness result: outcome (a) — step-65770 divergence closed. Harness now reaches step 66392 before hitting a new divergence (FLAGS category, ZF differs) at CS:IP=1FE0:7C9D on opcode bytes `01 C6 11 D7 89 76 D6 89` (`ADD SI, AX` at 1FE0:7C9D). "FreeDOS" banner now prints in the harness run. Becomes the next task.
Notes: Completed the LEA alignment started in EMU-21. Emulator side: added `s->seg_override_en = 1` as a side effect at the end of the LEA branch in `run.c` case 10, matching the reference's "LEA-via-segment-override" trick. Decremented to 0 before the next instruction's decode so no guest-visible effect. Harness side: taught `compare_states` in `harness/harness.c` to treat `ref seg_override == REG_ZERO (12)` as an equivalence-class match — REG_ZERO is the reference's sentinel for this LEA idiom and has no guest-observable meaning; our emulator has no such sentinel (we compute LEA offsets directly). Intel-behaviourally-equivalent divergence, two-part fix required. Reporting block in `report_divergence` left as-is (Option B per brief) — with Phase 3 change, DIV_PREFIX no longer fires for REG_ZERO-only divergences, so the misleading raw value line only appears in edge cases where DIV_PREFIX fires for another reason. Added the "LEA and the transient seg_override idiom" entry to `docs/notes/8086tiny-quirks.md`. Standalone `./emu86 reference/bios test/images/freedos.img` unchanged — FreeDOS banner reached, same divide-by-zero interrupt as before. Follow-up EMU-23 identified for `exec_lea` dead code in `opcodes/transfer.h` (either delete + its unit tests, or wire it into run.c case 10 and fix its latent hardcoded-DS bug). Next divergence (step 66392 FLAGS/ZF at ADD SI, AX) is the next task.

## EMU-23
Date: 2026-04-21
Status: PASS
Test results: 333 passed, 0 failed (down from 335 — removed `lea_basic` and `lea_no_memory_access` unit tests in test_transfer.c; 2946 assertions total, -2 from baseline). Runtime-path LEA coverage retained via EMU-22's three `run_lea_*` tests in test_run.c.
Harness result: unchanged (step 66392 FLAGS/ZF divergence at ADD SI, AX preserved — EMU-24 follow-up).
Notes: Resolved `exec_lea` dead-code finding from EMU-21 triage. Chose delete (Option A) — the runtime LEA in `run.c` case 10 is ~12 lines and depends on decode-table lookup (`read_table_sreg(s, seg_reg_idx)` with `seg_reg_idx = t->data[tbase+3][d->rm]`) that `exec_lea` had no access to; wiring `exec_lea` in would have required threading the decode tables through the function or duplicating the table lookup, a net increase for a short body. Removed `exec_lea` (and its pre-refactor comment block reasoning about LEA offset reconstruction) from `opcodes/transfer.h`. Removed `lea_basic` and `lea_no_memory_access` from `test_transfer.c` (including their `RUN_TEST` entries) — these were the only sites that exercised `exec_lea`, both with `SREG_DS = 0` which had masked the function's latent hardcoded-DS default-segment bug. Updated a stale comment in `test_run.c` that referenced `exec_lea` as "dead code on the runtime path". No runtime change — `run.c` case 10 never called `exec_lea`. Standalone `./emu86 reference/bios test/images/freedos.img` unchanged — FreeDOS banner reached, divide-by-zero loop as before.

## EMU-24
Date: 2026-04-21
Status: PASS
Test results: 1560 passed, 0 failed — no new unit tests (harness infrastructure)
Harness: still runs; heartbeats now visible in /tmp/emu86-harness/heartbeat.log
Notes: Added heartbeat log to harness.c. Default 1000-step cadence,
env-var override, truncate-on-startup, real-time flush. Sets up
EMU-25 investigation of the apparent hang around step 66k. Phase 4
integration tests: basic operation (file grows during run, last line
flushed on divergence exit), truncate-on-startup (sentinel line gone
after restart), HARNESS_HEARTBEAT_EVERY=100 override (~10x line rate),
HARNESS_HEARTBEAT_EVERY=0 disable (no writes), step-limit exit path
(final heartbeat preserved), divergence injection regression
(HARNESS_INJECT_DIVERGENCE_AT=500 still detected). Standalone emu86
build unchanged (harness-only change).

## EMU-25
Date: 2026-04-21
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
Deviation from brief: the script also injects prototype
declarations for the harness_* wrappers. Without them, implicit-int
declaration rules truncated size_t arguments on x86_64 and the
harness segfaulted on the first KEYBOARD_DRIVER call. The brief's
verbatim script omitted this; adding the prototypes is a strict
superset of the brief's intent and necessary for correctness.

## EMU-26
Date: 2026-04-21
Status: PASS
Test results: 1548 → 1560 (12 new MUL/IMUL flag tests; all pass)
Harness: advances past step 66,392; next divergence at step 69,489
         (AF mismatch at 1FE0:7D20 — EMU-27 territory).
Notes: Aligned SF/ZF/PF-after-MUL/IMUL to reference semantics.
Intel-undefined flags, similar shape to EMU-20's AF-on-shifts.
Preserved the reference's quirk where ZF looks at the full product
while SF/PF look at low halves. AF left untouched (the reference's
SZP-update path does not include AF). Added a regression-guard test
that pre-sets SF/ZF/PF to wrong values and verifies MUL overwrites
them — the exact shape of the bug EMU-26 fixes.

## EMU-27
Date: 2026-04-21
Status: PASS
Test results: 1560 → 1569 (9 new AF-preservation tests for
              AND/OR/XOR/TEST + regression guard; all pass)
Harness: advances past step 69,489; next divergence at step 70,424
         (OF mismatch at 1FE0:7D1D — EMU-28 territory).
Notes: Removed explicit AF clear from set_flags_logic. Another
Intel-undefined case, same shape as EMU-20 (shifts), EMU-26 (MUL).
The pattern is clear: when Intel says "undefined", the reference's
answer is typically "leave alone" — our job is to match that even
though "clear it" is equally Intel-legal.

## EMU-28
Date: 2026-04-22
Status: PASS
Test results: unchanged (harness infrastructure only)
Harness: divergence reports now include pre-step state via
rotating-buffer snapshots. Divergence at step 70,424 still
exists (EMU-29 territory); its pre-step CS:IP is now visible
directly in the report (1FE0:7D1B, opcode D3 E8 = SHR AX,CL).
Cross-checked against HARNESS_STEP_LIMIT=70423 — matches.
Notes: User suggested buffer-rotation optimisation — small
buffers per side, pointer swap at clean-step boundary, so pre
state is free at divergence time.

## EMU-29
Date: 2026-04-22
Status: PASS
Test results: test_shift 77 → 102 (15 new OF-for-count>1 tests
              across SHL/SHR/SAR/ROL/ROR, 25 new assertions; full
              suite green, 0 failures).
Harness: advances past step 70,424; next divergence at step
         1,103,526 (AX register mismatch at 9001:CBA4, opcode
         8B 46 06 = MOV AX,[BP+6] — EMU-30 territory).
Notes: Dropped "if count == 1" guards around OF updates in SHL,
SHR, SAR, ROL, ROR. Formulas unchanged — they were already
correct for count=1. Fourth Intel-undefined-flag alignment
(after EMU-20, EMU-26, EMU-27). The pattern — Intel undefined,
reference has specific behaviour, align — is now firm; a later
task will consolidate into docs.
