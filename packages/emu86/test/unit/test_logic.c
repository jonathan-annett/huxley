#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/logic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;

static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!state) { fprintf(stderr, "alloc failed\n"); exit(1); }
    emu86_init(state);
}
static void teardown(void) { free(state); state = NULL; }

static DecodeContext byte_op(uint8_t rm, uint8_t reg) {
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = rm; d.reg = reg; d.operand_width = 0; d.direction = 0;
    return d;
}
static DecodeContext word_op(uint8_t rm, uint8_t reg) {
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = rm; d.reg = reg; d.operand_width = 1; d.direction = 0;
    return d;
}
static DecodeContext unary_byte(uint8_t rm) {
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = rm; d.operand_width = 0;
    return d;
}
static DecodeContext unary_word(uint8_t rm) {
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = rm; d.operand_width = 1;
    return d;
}

/* === AND === */

TEST(and_byte_basic) {
    setup();
    state->regs[REG_AX] = 0xFF; state->regs[REG_BX] = 0x0F;
    DecodeContext d = byte_op(0, 3);
    exec_and(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x0F);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(and_byte_zero) {
    setup();
    state->regs[REG_AX] = 0xAA; state->regs[REG_BX] = 0x55;
    DecodeContext d = byte_op(0, 3);
    exec_and(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(and_word_basic) {
    setup();
    state->regs[REG_AX] = 0xFF00; state->regs[REG_BX] = 0x0FF0;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_and(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0F00);
    teardown();
}

TEST(and_clears_cf_of) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_OF);
    state->regs[REG_AX] = 0xFF; state->regs[REG_BX] = 0xFF;
    DecodeContext d = byte_op(0, 3);
    exec_and(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

/* === OR === */

TEST(or_byte_basic) {
    setup();
    state->regs[REG_AX] = 0xF0; state->regs[REG_BX] = 0x0F;
    DecodeContext d = byte_op(0, 3);
    exec_or(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    teardown();
}

TEST(or_byte_zero) {
    setup();
    state->regs[REG_AX] = 0x00; state->regs[REG_BX] = 0x00;
    DecodeContext d = byte_op(0, 3);
    exec_or(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(or_word_basic) {
    setup();
    state->regs[REG_AX] = 0xFF00; state->regs[REG_BX] = 0x00FF;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_or(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0xFFFF);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    teardown();
}

TEST(or_clears_cf_of) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_OF);
    state->regs[REG_AX] = 0x01; state->regs[REG_BX] = 0x01;
    DecodeContext d = byte_op(0, 3);
    exec_or(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

/* === XOR === */

TEST(xor_byte_basic) {
    setup();
    state->regs[REG_AX] = 0xFF; state->regs[REG_BX] = 0x0F;
    DecodeContext d = byte_op(0, 3);
    exec_xor(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0xF0);
    teardown();
}

TEST(xor_self_zeros) {
    setup();
    state->regs[REG_AX] = 0x1234;
    DecodeContext d = word_op(REG_AX, REG_AX);
    exec_xor(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(xor_word_basic) {
    setup();
    state->regs[REG_AX] = 0xAAAA; state->regs[REG_BX] = 0x5555;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_xor(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0xFFFF);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    teardown();
}

/* === NOT === */

TEST(not_byte_basic) {
    setup();
    state->regs[REG_AX] = 0x00;
    DecodeContext d = unary_byte(0);
    exec_not(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    teardown();
}

TEST(not_byte_ff) {
    setup();
    state->regs[REG_AX] = 0xFF;
    DecodeContext d = unary_byte(0);
    exec_not(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    teardown();
}

TEST(not_word_basic) {
    setup();
    state->regs[REG_AX] = 0xAAAA;
    DecodeContext d = unary_word(REG_AX);
    exec_not(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x5555);
    teardown();
}

TEST(not_preserves_flags) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    set_flag(state, FLAG_SF); set_flag(state, FLAG_OF);
    uint16_t before = state->flags;
    state->regs[REG_AX] = 0x42;
    DecodeContext d = unary_byte(0);
    exec_not(state, &d);
    ASSERT_EQ(state->flags, before);
    teardown();
}

/* === TEST === */

TEST(test_byte_match) {
    setup();
    state->regs[REG_AX] = 0xFF; state->regs[REG_BX] = 0x0F;
    DecodeContext d = byte_op(0, 3);
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(read_reg8(state, 0), 0xFF); /* unchanged */
    teardown();
}

TEST(test_byte_no_match) {
    setup();
    state->regs[REG_AX] = 0xF0; state->regs[REG_BX] = 0x0F;
    DecodeContext d = byte_op(0, 3);
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(read_reg8(state, 0), 0xF0); /* unchanged */
    teardown();
}

TEST(test_preserves_operands) {
    setup();
    state->regs[REG_AX] = 0x42; state->regs[REG_BX] = 0x42;
    DecodeContext d = byte_op(0, 3);
    exec_test(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    ASSERT_EQ(read_reg8(state, 3), 0x42);
    teardown();
}

TEST(test_clears_cf_of) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_OF);
    state->regs[REG_AX] = 0xFF; state->regs[REG_BX] = 0xFF;
    DecodeContext d = byte_op(0, 3);
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(test_word_basic) {
    setup();
    state->regs[REG_AX] = 0x8000; state->regs[REG_BX] = 0x8000;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    teardown();
}

TEST(test_parity) {
    setup();
    state->regs[REG_AX] = 0x03; state->regs[REG_BX] = 0xFF;
    DecodeContext d = byte_op(0, 3);
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1); /* 0x03: 2 bits, even */

    state->regs[REG_AX] = 0x01; state->regs[REG_BX] = 0xFF;
    exec_test(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_PF), 0); /* 0x01: 1 bit, odd */
    teardown();
}

int main(void) {
    printf("test_logic:\n");
    RUN_TEST(and_byte_basic); RUN_TEST(and_byte_zero);
    RUN_TEST(and_word_basic); RUN_TEST(and_clears_cf_of);
    RUN_TEST(or_byte_basic); RUN_TEST(or_byte_zero);
    RUN_TEST(or_word_basic); RUN_TEST(or_clears_cf_of);
    RUN_TEST(xor_byte_basic); RUN_TEST(xor_self_zeros);
    RUN_TEST(xor_word_basic);
    RUN_TEST(not_byte_basic); RUN_TEST(not_byte_ff);
    RUN_TEST(not_word_basic); RUN_TEST(not_preserves_flags);
    RUN_TEST(test_byte_match); RUN_TEST(test_byte_no_match);
    RUN_TEST(test_preserves_operands); RUN_TEST(test_clears_cf_of);
    RUN_TEST(test_word_basic); RUN_TEST(test_parity);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
