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
