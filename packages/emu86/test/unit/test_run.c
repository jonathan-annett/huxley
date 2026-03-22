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
    RUN_TEST(run_call_ret);
    RUN_TEST(run_software_int);
    RUN_TEST(run_interrupt_when_if_clear);
    RUN_TEST(run_rep_movsb);
    RUN_TEST(run_fibonacci);
    RUN_TEST(run_memory_fill);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
