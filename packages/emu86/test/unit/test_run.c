#include "harness.h"
#include "../../src/emulator/run.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/platform.h"
#include "../../src/emulator/tables.h"
#include "../../src/emulator/decode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static Emu86Tables tables;
static Emu86Platform platform;
static Emu86YieldInfo yield;

/* Ring buffer backing storage */
static uint8_t cin_buf[256], cout_buf[256];
static uint32_t cin_head, cin_tail, cout_head, cout_tail;

static void setup(void)
{
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);

    /* Load BIOS */
    FILE *f = fopen("reference/bios", "rb");
    if (!f) { fprintf(stderr, "Cannot open reference/bios\n"); exit(1); }
    fread(state->mem + 0xF0100, 1, 0xFF00, f);
    fclose(f);
    emu86_load_tables(&tables, state);

    /* Set up platform with ring buffers */
    memset(&platform, 0, sizeof(platform));
    cin_head = cin_tail = cout_head = cout_tail = 0;
    memset(cin_buf, 0, sizeof(cin_buf));
    memset(cout_buf, 0, sizeof(cout_buf));
    platform.console_in.buf = cin_buf;
    platform.console_in.size = 256;
    platform.console_in.head = &cin_head;
    platform.console_in.tail = &cin_tail;
    platform.console_out.buf = cout_buf;
    platform.console_out.size = 256;
    platform.console_out.head = &cout_head;
    platform.console_out.tail = &cout_tail;

    /* Set test CS:IP and stack */
    state->sregs[SREG_CS] = 0x0000;
    state->ip = 0x7C00;
    state->sregs[SREG_SS] = 0x0000;
    state->regs[REG_SP] = 0x7C00;
    state->sregs[SREG_DS] = 0x0000;
    state->sregs[SREG_ES] = 0x0000;

    memset(&yield, 0, sizeof(yield));
}

static void teardown(void) { free(state); state = NULL; }

/* Helper: place code at CS:IP offset */
static void place(uint16_t offset, const uint8_t *bytes, int len) {
    uint32_t addr = segoff_to_linear(state->sregs[SREG_CS], offset);
    memcpy(state->mem + addr, bytes, len);
}

/* === Basic execution === */

TEST(run_single_nop)
{
    setup();
    uint8_t code[] = { 0x90, 0xF4 }; /* NOP, HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT(yield.cycles_used > 0);
    ASSERT_EQ(state->ip, 0x7C02); /* IP past HLT (IP advances after execution) */
    teardown();
}

TEST(run_mov_and_halt)
{
    setup();
    uint8_t code[] = { 0xB8, 0x34, 0x12, 0xF4 }; /* MOV AX,0x1234; HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    teardown();
}

TEST(run_add_two_registers)
{
    setup();
    uint8_t code[] = {
        0xB8, 0x05, 0x00,  /* MOV AX, 5 */
        0xBB, 0x03, 0x00,  /* MOV BX, 3 */
        0x01, 0xD8,         /* ADD AX, BX */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 8);
    teardown();
}

TEST(run_budget_exhaustion)
{
    setup();
    uint8_t code[] = { 0xEB, 0xFE }; /* JMP short $-2 (infinite loop) */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_BUDGET);
    ASSERT(yield.cycles_used >= 100);
    teardown();
}

TEST(run_exit_condition)
{
    setup();
    uint8_t code[] = { 0xEA, 0x00, 0x00, 0x00, 0x00 }; /* JMP FAR 0000:0000 */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_EXIT);
    teardown();
}

/* === Step single === */

TEST(step_single_executes_one)
{
    setup();
    uint8_t code[] = {
        0xB8, 0x34, 0x12,  /* MOV AX, 0x1234 */
        0xBB, 0x78, 0x56,  /* MOV BX, 0x5678 */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));

    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    ASSERT_EQ(state->regs[REG_BX], 0x0000); /* not yet executed */

    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->regs[REG_BX], 0x5678);

    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->halted, 1);
    teardown();
}

TEST(step_single_returns_cycles)
{
    setup();
    uint8_t code[] = { 0x90 }; /* NOP */
    place(0x7C00, code, sizeof(code));
    int cycles = emu86_step_single(state, &platform, &tables);
    ASSERT(cycles > 0);
    teardown();
}

/* === Arithmetic in run loop === */

TEST(run_subtract_and_compare)
{
    setup();
    uint8_t code[] = {
        0xB8, 0x0A, 0x00,  /* MOV AX, 10 */
        0xBB, 0x03, 0x00,  /* MOV BX, 3 */
        0x29, 0xD8,         /* SUB AX, BX */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(state->regs[REG_AX], 7);
    ASSERT_EQ(state->flags & FLAG_CF, 0);
    teardown();
}

TEST(run_compare_and_jump)
{
    setup();
    uint8_t code[] = {
        0xB8, 0x05, 0x00,  /* MOV AX, 5 */
        0x3D, 0x05, 0x00,  /* CMP AX, 5 */
        0x74, 0x04,         /* JZ +4 (skip to target) */
        0xBB, 0x00, 0x00,  /* MOV BX, 0 (skipped) */
        0xF4,               /* HLT (skipped) */
        0xBB, 0x01, 0x00,  /* target: MOV BX, 1 */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(state->regs[REG_BX], 1);
    teardown();
}

TEST(run_compare_and_no_jump)
{
    setup();
    uint8_t code[] = {
        0xB8, 0x05, 0x00,  /* MOV AX, 5 */
        0x3D, 0x03, 0x00,  /* CMP AX, 3 */
        0x74, 0x04,         /* JZ +4 (not taken since 5!=3) */
        0xBB, 0x02, 0x00,  /* MOV BX, 2 */
        0xF4,               /* HLT */
        0xBB, 0x01, 0x00,  /* target: MOV BX, 1 (not reached) */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(state->regs[REG_BX], 2);
    teardown();
}

/* === Loop === */

TEST(run_loop_counter)
{
    setup();
    uint8_t code[] = {
        0xB9, 0x05, 0x00,  /* MOV CX, 5 */
        0xB8, 0x00, 0x00,  /* MOV AX, 0 */
        0x40,               /* loop_start: INC AX */
        0xE2, 0xFD,         /* LOOP loop_start (-3) */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 200, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 5);
    ASSERT_EQ(state->regs[REG_CX], 0);
    teardown();
}

/* === Stack === */

TEST(run_push_pop)
{
    setup();
    uint8_t code[] = {
        0xB8, 0xEF, 0xBE,  /* MOV AX, 0xBEEF */
        0x50,               /* PUSH AX */
        0xB8, 0x00, 0x00,  /* MOV AX, 0 */
        0x58,               /* POP AX */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(state->regs[REG_AX], 0xBEEF);
    teardown();
}

/* Regression: PUSH/POP sreg dispatch must map original's extra (8..11)
 * to our sregs index (0..3). A prior bug passed extra straight through,
 * so POP DS wrote out of bounds and DS stayed zero — which broke BIOS
 * IVT setup (mov cx, [itbl_size] with DS=0 read zeros). */
TEST(run_push_pop_sreg_dispatch)
{
    setup();
    state->sregs[SREG_ES] = 0x1234;
    state->sregs[SREG_DS] = 0x5678;
    uint8_t code[] = {
        0x06,   /* PUSH ES (0x1234) */
        0x1F,   /* POP DS  -> DS should become 0x1234 */
        0x1E,   /* PUSH DS (now 0x1234) */
        0x07,   /* POP ES  -> ES should still be 0x1234 */
        0xF4,   /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->sregs[SREG_DS], 0x1234);
    ASSERT_EQ(state->sregs[SREG_ES], 0x1234);
    teardown();
}

TEST(run_call_ret)
{
    setup();
    /* Main: CALL subroutine / HLT
     * Subroutine: MOV AX, 0x42 / RET
     * The CALL offset is relative to next IP. CALL is at 7C00, 3 bytes,
     * so next IP = 7C03. Subroutine at 7C04 (offset = 1).
     * Wait: HLT is at 7C03, subroutine at 7C04.
     * CALL near rel16: E8 xx xx. Offset = 7C04 - (7C00+3) = 1 */
    uint8_t code[] = {
        0xE8, 0x01, 0x00,  /* CALL +1 (to 0x7C04) */
        0xF4,               /* HLT (return here) */
        0xB8, 0x42, 0x00,  /* subroutine: MOV AX, 0x42 */
        0xC3                /* RET */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(state->regs[REG_AX], 0x42);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    teardown();
}

/* === Interrupts === */

TEST(run_software_int)
{
    setup();
    /* Set up IVT entry for INT 0x20 at vector 0x20*4 = 0x80 */
    /* Handler at 0x8000: MOV BX, 0xFF / IRET */
    uint8_t handler[] = { 0xBB, 0xFF, 0x00, 0xCF }; /* MOV BX,0xFF; IRET */
    place(0x8000, handler, sizeof(handler));
    /* IVT: IP=0x8000, CS=0x0000 */
    state->mem[0x80] = 0x00; state->mem[0x81] = 0x80; /* IP = 0x8000 */
    state->mem[0x82] = 0x00; state->mem[0x83] = 0x00; /* CS = 0x0000 */

    uint8_t code[] = {
        0xBB, 0x00, 0x00,  /* MOV BX, 0 */
        0xCD, 0x20,         /* INT 0x20 */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    state->flags |= FLAG_IF;
    emu86_run(state, &platform, &tables, 200, &yield);
    ASSERT_EQ(state->regs[REG_BX], 0xFF);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    teardown();
}

TEST(run_interrupt_when_if_clear)
{
    setup();
    state->int_pending = 1;
    state->int_vector = 0x20;
    state->flags &= ~FLAG_IF; /* IF=0: interrupts disabled */

    /* Handler that sets BX=0xFF */
    uint8_t handler[] = { 0xBB, 0xFF, 0x00, 0xCF };
    place(0x8000, handler, sizeof(handler));
    state->mem[0x80] = 0x00; state->mem[0x81] = 0x80;
    state->mem[0x82] = 0x00; state->mem[0x83] = 0x00;

    uint8_t code[] = {
        0xBB, 0x00, 0x00,  /* MOV BX, 0 */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    /* Interrupt should NOT have fired (IF=0) */
    ASSERT_EQ(state->regs[REG_BX], 0);
    ASSERT_EQ(state->int_pending, 1); /* still pending */
    teardown();
}

/* === REP string ops === */

TEST(run_rep_movsb)
{
    setup();
    /* Source data at DS:0x100 */
    for (int i = 0; i < 10; i++)
        state->mem[0x100 + i] = (uint8_t)(0xA0 + i);

    uint8_t code[] = {
        0xBE, 0x00, 0x01,  /* MOV SI, 0x100 */
        0xBF, 0x00, 0x02,  /* MOV DI, 0x200 */
        0xB9, 0x0A, 0x00,  /* MOV CX, 10 */
        0xF3, 0xA4,         /* REP MOVSB */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 500, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(state->mem[0x200 + i], (uint8_t)(0xA0 + i));
    ASSERT_EQ(state->regs[REG_CX], 0);
    teardown();
}

/* === Fibonacci === */

TEST(run_fibonacci)
{
    setup();
    uint8_t code[] = {
        0xB9, 0x0A, 0x00,  /* MOV CX, 10 */
        0xB8, 0x00, 0x00,  /* MOV AX, 0 */
        0xBB, 0x01, 0x00,  /* MOV BX, 1 */
        /* loop: */
        0x89, 0xC2,         /* MOV DX, AX */
        0x01, 0xD8,         /* ADD AX, BX */
        0x89, 0xD3,         /* MOV BX, DX */
        0xE2, 0xF8,         /* LOOP -8 (back to MOV DX,AX) */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 500, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 55);
    teardown();
}

/* === ALU r/m, imm (0x80-0x83) — regression tests for EMU-13 ===
 *
 * The decoder computes inst_length linearly via `iw_size*(operand_width+1)`,
 * which for this group always counts 2 bytes of immediate (or 1 for w=0).
 * But the real immediate size depends on the sign_ext bit:
 *   0x80 (w=0,d=0) → 1-byte imm       (decoder counted 1; needs 0 delta)
 *   0x81 (w=1,d=0) → 2-byte imm       (decoder counted 2; needs 0 delta)
 *   0x82 (w=0,d=1) → 1-byte imm       (decoder counted 1; needs 0 delta)
 *   0x83 (w=1,d=1) → 1-byte imm s-ext (decoder counted 2; needs -1 delta)
 * The pre-fix code added `sign_ext ? 1 : 2` on TOP of the decoder's count,
 * overcounting by exactly (operand_width+1). These tests each fail under
 * the old code (IP skips past HLT, yielding BUDGET instead of HALTED).
 */

TEST(run_alu_imm_80_add_al_reg)
{
    setup();
    uint8_t code[] = { 0x80, 0xC0, 0x42, 0xF4 }; /* ADD AL, 0x42; HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x42);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_alu_imm_81_add_ax_reg)
{
    setup();
    uint8_t code[] = { 0x81, 0xC0, 0x34, 0x12, 0xF4 }; /* ADD AX, 0x1234; HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    ASSERT_EQ(state->ip, 0x7C05);
    teardown();
}

TEST(run_alu_imm_82_add_bl_reg)
{
    /* 0x82 is an undocumented alias of 0x80 that must still decode correctly
     * — a future regression could easily skip this opcode. */
    setup();
    uint8_t code[] = { 0x82, 0xC3, 0xFF, 0xF4 }; /* ADD BL, 0xFF; HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ((uint8_t)state->regs[REG_BX], 0xFF);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_alu_imm_83_add_ax_signext)
{
    setup();
    uint8_t code[] = { 0x83, 0xC0, 0x01, 0xF4 }; /* ADD AX, 1 (sign-ext); HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 1);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_alu_imm_83_sub_ax)
{
    /* Exercises the `reg` (sub-function) dispatch within the 0x80-0x83 family:
     * modrm=E8 → reg=5 = SUB, not ADD. */
    setup();
    state->regs[REG_AX] = 5;
    uint8_t code[] = { 0x83, 0xE8, 0x01, 0xF4 }; /* SUB AX, 1; HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 4);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_alu_imm_81_mem_direct)
{
    /* Memory operand, mod=0 rm=6 → direct 16-bit address (no disp register). */
    setup();
    uint8_t code[] = { 0x81, 0x06, 0x00, 0x02, 0x34, 0x12, 0xF4 };
    /* ADD word [0x0200], 0x1234; HLT — 6 bytes for the ADD, 1 for HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->mem[0x0200], 0x34);
    ASSERT_EQ(state->mem[0x0201], 0x12);
    ASSERT_EQ(state->ip, 0x7C07);
    teardown();
}

TEST(run_alu_imm_80_mem_disp8)
{
    /* Memory operand, mod=1 rm=7 → [BX+disp8]. */
    setup();
    state->regs[REG_BX] = 0x0300;
    uint8_t code[] = { 0x80, 0x47, 0x05, 0x10, 0xF4 };
    /* ADD byte [BX+5], 0x10; HLT — 4 bytes for the ADD, 1 for HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->mem[0x0305], 0x10);
    ASSERT_EQ(state->ip, 0x7C05);
    teardown();
}

TEST(run_alu_imm_81_mem_disp16)
{
    /* Memory operand, mod=2 rm=7 → [BX+disp16]. */
    setup();
    state->regs[REG_BX] = 0x0200;
    uint8_t code[] = { 0x81, 0x87, 0x00, 0x01, 0x34, 0x12, 0xF4 };
    /* ADD word [BX+0x0100], 0x1234; HLT — 6 bytes for the ADD, 1 for HLT */
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->mem[0x0300], 0x34);
    ASSERT_EQ(state->mem[0x0301], 0x12);
    ASSERT_EQ(state->ip, 0x7C07);
    teardown();
}

/* === Shift/rotate r/m, imm (0xC0/0xC1) — regression tests for EMU-15 ===
 *
 * For opcodes 0xC0/0xC1 (80186 shift-by-immediate), the BIOS tables give
 * base_size=3 (already counting opcode + modrm + imm8). The decoder's
 * linear length formula yields `3 + modrm_disp + 0 * (w+1) = 3 + modrm_disp`,
 * which is the correct instruction length. Pre-fix case 12 then did
 * `d->inst_length++` in the `extra` branch, over-advancing IP by one byte
 * on every 0xC0/0xC1 execution. The FreeDOS BIOS hit this at F000:0D4D
 * running `C1 E0 09` (SHL AX, 9) and skipped the following `0F` byte,
 * breaking the extended_read_disk hook (EMU-14 triage).
 *
 * These tests drive each of 0xC0/0xC1 through emu86_run() and assert that
 * HLT is reached at the expected IP — i.e. inst_length advanced by exactly
 * the right amount. Pre-fix they hit BUDGET or EXIT (skipping HLT into
 * zeroed memory); post-fix they HALT cleanly.
 */

TEST(run_shift_imm_c1_shl_ax_reg)
{
    /* C1 E0 09 F4 — SHL AX, 9; HLT. The exact BIOS-triggering instruction. */
    setup();
    state->regs[REG_AX] = 0x0001;
    uint8_t code[] = { 0xC1, 0xE0, 0x09, 0xF4 };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 0x0200);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_shift_imm_c1_shr_ax_reg)
{
    /* C1 E8 01 F4 — SHR AX, 1; HLT. modrm=E8 → reg=5 (SHR), not SHL (reg=4).
     * Exercises a different sub-function within case 12's extra=1 branch. */
    setup();
    state->regs[REG_AX] = 0x0002;
    uint8_t code[] = { 0xC1, 0xE8, 0x01, 0xF4 };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 0x0001);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_shift_imm_c0_rol_al_reg)
{
    /* C0 C0 04 F4 — ROL AL, 4; HLT. The 8-bit (0xC0) form, w=0.
     * modrm=C0 → mod=3, reg=0 (ROL), rm=0 (AL). */
    setup();
    state->regs[REG_AX] = 0x0012;  /* AL = 0x12 */
    uint8_t code[] = { 0xC0, 0xC0, 0x04, 0xF4 };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x21);
    ASSERT_EQ(state->ip, 0x7C04);
    teardown();
}

TEST(run_shift_imm_c1_shl_mem_direct)
{
    /* C1 26 04 02 04 F4 — SHL word [0x0204], 4; HLT.
     * Memory operand via mod=0 rm=6 (direct 16-bit address) — exercises the
     * path where modrm displacement adds to inst_length (2 extra bytes for
     * the disp16 in this case). Pre-fix: inst_length = 3+2+1 = 6, IP skips
     * past HLT. Post-fix: inst_length = 3+2 = 5, IP lands on HLT.
     *
     * Note: our case 12 reads the shift count from `d->data1 & 0xFF`, which
     * for mod=0 rm=6 is the low byte of the disp16. Choosing disp=0x0204
     * makes that low byte equal the intended imm (4), so the shift count
     * also happens to be correct and we can assert the memory result. */
    setup();
    state->mem[0x0204] = 0x34;
    state->mem[0x0205] = 0x12;
    uint8_t code[] = { 0xC1, 0x26, 0x04, 0x02, 0x04, 0xF4 };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 100, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->mem[0x0204], 0x40);
    ASSERT_EQ(state->mem[0x0205], 0x23);
    ASSERT_EQ(state->ip, 0x7C06);
    teardown();
}

/* === Timer tick cadence (EMU-19) ===
 *
 * The timer polling check at run.c:587 fires int8_asap once every 20000
 * instructions, matching reference/8086tiny.c's KEYBOARD_TIMER_UPDATE_DELAY.
 * A prior bug used `(inst_count & 0x4FFF) == 0`, which fires at 0x1000,
 * 0x2000, 0x3000, 0x4000, 0x8000, ... — a bitmask pattern, not a regular
 * cadence — and caused a harness divergence at step 4096 (EMU-16/18).
 *
 * These tests run a tight JMP $-2 loop (one instruction per step) with
 * FLAG_IF=0 so the interrupt-service path doesn't clear int8_asap.
 * int8_asap is cleared between sampled runs so each rising edge can be
 * observed.
 */

/* Runs exactly `steps` instructions of JMP $-2 and counts how many times
 * int8_asap transitioned from 0 to 1. Assumes FLAG_IF is already clear.
 * Clears int8_asap after each detected rising edge so subsequent edges
 * are observable. */
static int run_and_count_timer_ticks(int steps)
{
    int ticks = 0;
    for (int i = 0; i < steps; i++) {
        state->int8_asap = 0;
        emu86_run(state, &platform, &tables, 4, &yield);
        if (state->int8_asap) ticks++;
    }
    return ticks;
}

static void timer_setup(void)
{
    setup();
    uint8_t code[] = { 0xEB, 0xFE }; /* JMP $-2 (1 instruction, infinite loop) */
    place(0x7C00, code, sizeof(code));
    state->flags &= ~FLAG_IF;       /* don't service int8_asap */
    state->int8_asap = 0;
    state->inst_count = 0;
}

TEST(timer_first_tick_at_20000)
{
    /* First rising edge of int8_asap must occur at inst_count == 20000,
     * not at 4096 (the buggy bitmask cadence fires here). */
    timer_setup();

    int ticks_through_19999 = run_and_count_timer_ticks(19999);
    ASSERT_EQ(ticks_through_19999, 0);
    ASSERT_EQ(state->inst_count, 19999u);
    ASSERT_EQ(state->int8_asap, 0);

    int ticks_on_20000th = run_and_count_timer_ticks(1);
    ASSERT_EQ(ticks_on_20000th, 1);
    ASSERT_EQ(state->inst_count, 20000u);
    ASSERT_EQ(state->int8_asap, 1);

    teardown();
}

TEST(timer_not_firing_at_4096)
{
    /* The buggy bitmask `inst_count & 0x4FFF == 0` fires at 4096. The
     * correct `% 20000` cadence must not. This is the exact instruction
     * count at which the harness previously diverged. */
    timer_setup();

    int ticks = run_and_count_timer_ticks(4096);
    ASSERT_EQ(ticks, 0);
    ASSERT_EQ(state->inst_count, 4096u);
    ASSERT_EQ(state->int8_asap, 0);

    teardown();
}

TEST(timer_cadence_count_exact_multiples)
{
    /* For N = 20000 * k the count of rising edges should equal exactly k. */
    timer_setup();

    int ticks = run_and_count_timer_ticks(20000 * 3);
    ASSERT_EQ(ticks, 3);
    ASSERT_EQ(state->inst_count, 60000u);

    teardown();
}

TEST(timer_cadence_count_partial_window)
{
    /* For N = 20000 * k + r (0 < r < 20000) the count should still be
     * exactly k — the next edge hasn't fired yet. */
    timer_setup();

    int ticks = run_and_count_timer_ticks(20000 * 2 + 1234);
    ASSERT_EQ(ticks, 2);
    ASSERT_EQ(state->inst_count, 41234u);

    teardown();
}

TEST(timer_cadence_regular_intervals)
{
    /* Intervals between consecutive rising edges must all equal 20000. */
    timer_setup();

    uint32_t edges[3] = {0, 0, 0};
    int edge_idx = 0;
    for (int i = 0; i < 20000 * 3 + 5; i++) {
        state->int8_asap = 0;
        emu86_run(state, &platform, &tables, 4, &yield);
        if (state->int8_asap && edge_idx < 3) {
            edges[edge_idx++] = state->inst_count;
        }
    }
    ASSERT_EQ(edge_idx, 3);
    ASSERT_EQ(edges[0], 20000u);
    ASSERT_EQ(edges[1] - edges[0], 20000u);
    ASSERT_EQ(edges[2] - edges[1], 20000u);

    teardown();
}

/* === LEA (runtime path — EMU-22) === */

/* LEA sets seg_override_en=1 as a side effect (EMU-22 alignment with the
 * reference's LEA-via-segment-override trick). Regression guard through the
 * runtime dispatch path in run.c case 10. */
TEST(run_lea_sets_seg_override_en)
{
    setup();
    /* LEA BX, [DI+0x10] → 8D 5D 10. Followed by HLT. */
    uint8_t code[] = { 0x8D, 0x5D, 0x10, 0xF4 };
    place(0x7C00, code, sizeof(code));
    state->regs[REG_DI] = 0x0040;
    state->seg_override_en = 0;
    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->seg_override_en, 1);
    teardown();
}

/* Regression guard: the seg_override_en side effect must not change LEA's
 * destination-register result. */
TEST(run_lea_result_unchanged)
{
    setup();
    uint8_t code[] = { 0x8D, 0x5D, 0x10, 0xF4 }; /* LEA BX, [DI+0x10] */
    place(0x7C00, code, sizeof(code));
    state->regs[REG_DI] = 0x0040;
    state->regs[REG_BX] = 0xDEAD;
    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->regs[REG_BX], 0x0050); /* DI + 0x10 */
    teardown();
}

/* Regression guard: LEA doesn't modify flags. */
TEST(run_lea_flags_unchanged)
{
    setup();
    uint8_t code[] = { 0x8D, 0x5D, 0x10, 0xF4 }; /* LEA BX, [DI+0x10] */
    place(0x7C00, code, sizeof(code));
    state->regs[REG_DI] = 0x0040;
    /* Set a distinctive pattern across all arith flags. */
    state->flags = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    uint16_t before = state->flags;
    emu86_step_single(state, &platform, &tables);
    ASSERT_EQ(state->flags, before);
    teardown();
}

/* EMU-30 regression guard: LEA's seg_override_en=1 side effect must not
 * leak into the next instruction's decode. Before EMU-30 the main loop
 * decremented seg_override_en after decode, so the next step's decode
 * saw the stale 1 and applied a segment override to its effective-
 * address computation. Caught in FreeDOS at step 1,103,526 as a
 * MOV AX, [BP+6] reading from the wrong segment. */
TEST(run_lea_then_mov_no_override_leak)
{
    setup();
    uint8_t code[] = {
        0x8D, 0x5D, 0x10,   /* LEA BX, [DI+0x10] — sets seg_override_en=1 */
        0x8B, 0x46, 0x06,   /* MOV AX, [BP+0x06] — BP default seg is SS */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));

    /* Distinct segments so an override leak is observable.
     *   SS:[BP+6] linear = 0x04006 → 0x0516 (correct, SS is default for BP)
     *   ES:[BP+6] linear = 0x14006 → 0x8002 (what a leaked override reads) */
    state->sregs[SREG_SS] = 0x0000;
    state->sregs[SREG_ES] = 0x1000;
    state->regs[REG_BP]   = 0x4000;
    state->regs[REG_DI]   = 0x0040;
    state->mem[0x04006] = 0x16;
    state->mem[0x04007] = 0x05;
    state->mem[0x14006] = 0x02;
    state->mem[0x14007] = 0x80;
    /* LEA sets seg_override_en but not seg_override. Pin seg_override
     * to ES so the leaked-override path reads from ES, making the
     * test sensitive regardless of the pre-LEA default. */
    state->seg_override = SREG_ES;
    state->seg_override_en = 0;

    emu86_run(state, &platform, &tables, 2000, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    ASSERT_EQ(state->regs[REG_AX], 0x0516);
    teardown();
}

/* === Memory fill === */

TEST(run_memory_fill)
{
    setup();
    uint8_t code[] = {
        0xB8, 0xFF, 0x00,  /* MOV AX, 0xFF (AL=0xFF) */
        0xB9, 0x00, 0x01,  /* MOV CX, 256 */
        0xBF, 0x00, 0x10,  /* MOV DI, 0x1000 */
        0xF3, 0xAA,         /* REP STOSB */
        0xF4                /* HLT */
    };
    place(0x7C00, code, sizeof(code));
    emu86_run(state, &platform, &tables, 2000, &yield);
    ASSERT_EQ(yield.reason, EMU86_YIELD_HALTED);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(state->mem[0x1000 + i], 0xFF);
    teardown();
}

int main(void)
{
    printf("test_run:\n");

    RUN_TEST(run_single_nop);
    RUN_TEST(run_mov_and_halt);
    RUN_TEST(run_add_two_registers);
    RUN_TEST(run_budget_exhaustion);
    RUN_TEST(run_exit_condition);
    RUN_TEST(step_single_executes_one);
    RUN_TEST(step_single_returns_cycles);
    RUN_TEST(run_subtract_and_compare);
    RUN_TEST(run_compare_and_jump);
    RUN_TEST(run_compare_and_no_jump);
    RUN_TEST(run_loop_counter);
    RUN_TEST(run_push_pop);
    RUN_TEST(run_push_pop_sreg_dispatch);
    RUN_TEST(run_call_ret);
    RUN_TEST(run_software_int);
    RUN_TEST(run_interrupt_when_if_clear);
    RUN_TEST(run_rep_movsb);
    RUN_TEST(run_fibonacci);
    RUN_TEST(run_alu_imm_80_add_al_reg);
    RUN_TEST(run_alu_imm_81_add_ax_reg);
    RUN_TEST(run_alu_imm_82_add_bl_reg);
    RUN_TEST(run_alu_imm_83_add_ax_signext);
    RUN_TEST(run_alu_imm_83_sub_ax);
    RUN_TEST(run_alu_imm_81_mem_direct);
    RUN_TEST(run_alu_imm_80_mem_disp8);
    RUN_TEST(run_alu_imm_81_mem_disp16);
    RUN_TEST(run_shift_imm_c1_shl_ax_reg);
    RUN_TEST(run_shift_imm_c1_shr_ax_reg);
    RUN_TEST(run_shift_imm_c0_rol_al_reg);
    RUN_TEST(run_shift_imm_c1_shl_mem_direct);
    RUN_TEST(timer_first_tick_at_20000);
    RUN_TEST(timer_not_firing_at_4096);
    RUN_TEST(timer_cadence_count_exact_multiples);
    RUN_TEST(timer_cadence_count_partial_window);
    RUN_TEST(timer_cadence_regular_intervals);
    RUN_TEST(run_lea_sets_seg_override_en);
    RUN_TEST(run_lea_result_unchanged);
    RUN_TEST(run_lea_flags_unchanged);
    RUN_TEST(run_lea_then_mov_no_override_leak);
    RUN_TEST(run_memory_fill);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
