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
