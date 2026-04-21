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