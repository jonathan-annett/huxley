#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/shift.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);
}
static void teardown(void) { free(state); state = NULL; }

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

/* === SHL === */

TEST(shl_byte_by_1) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(shl_byte_by_1_no_carry) {
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x02);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(shl_byte_by_4) {
    setup();
    state->regs[REG_AX] = 0x0F;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 4);
    ASSERT_EQ(read_reg8(state, 0), 0xF0);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(shl_word_by_1) {
    setup();
    state->regs[REG_AX] = 0x8000;
    DecodeContext d = unary_word(REG_AX);
    exec_shl(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(shl_of_set) {
    setup();
    state->regs[REG_AX] = 0x40; /* bit 6 set */
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1); /* CF(0) XOR MSB(1) = 1 */
    teardown();
}

TEST(shl_of_clear) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1); /* CF(1) XOR MSB(0) = 1 */
    teardown();
}

TEST(shl_sets_szp) {
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x02);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 0);
    teardown();
}

TEST(shl_count_zero_noop) {
    setup();
    state->regs[REG_AX] = 0x42;
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t flags_before = state->flags;
    DecodeContext d = unary_byte(0);
    exec_shl(state, &d, 0);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    ASSERT_EQ(state->flags, flags_before);
    teardown();
}

/* === SHR === */

TEST(shr_byte_by_1) {
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(shr_byte_by_1_no_carry) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x40);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(shr_byte_by_4) {
    setup();
    state->regs[REG_AX] = 0xF0;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 4);
    ASSERT_EQ(read_reg8(state, 0), 0x0F);
    teardown();
}

TEST(shr_word_by_1) {
    setup();
    state->regs[REG_AX] = 0x0001;
    DecodeContext d = unary_word(REG_AX);
    exec_shr(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(shr_of_set) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1); /* original MSB=1 */
    teardown();
}

TEST(shr_of_clear) {
    setup();
    state->regs[REG_AX] = 0x40;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0); /* original MSB=0 */
    teardown();
}

TEST(shr_fills_zero) {
    setup();
    state->regs[REG_AX] = 0xFF;
    DecodeContext d = unary_byte(0);
    exec_shr(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x7F);
    teardown();
}

/* === SAR === */

TEST(sar_byte_positive) {
    setup();
    state->regs[REG_AX] = 0x40;
    DecodeContext d = unary_byte(0);
    exec_sar(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x20);
    teardown();
}

TEST(sar_byte_negative) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_sar(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0xC0);
    teardown();
}

TEST(sar_byte_negative_by_4) {
    setup();
    state->regs[REG_AX] = 0xF0;
    DecodeContext d = unary_byte(0);
    exec_sar(state, &d, 4);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    teardown();
}

TEST(sar_word_negative) {
    setup();
    state->regs[REG_AX] = 0x8000;
    DecodeContext d = unary_word(REG_AX);
    exec_sar(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0xC000);
    teardown();
}

TEST(sar_of_zero_for_count_1) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_sar(state, &d, 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(sar_preserves_sign) {
    setup();
    state->regs[REG_AX] = 0xFF;
    DecodeContext d = unary_byte(0);
    exec_sar(state, &d, 7);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    teardown();
}

/* === ROL === */

TEST(rol_byte_by_1) {
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_rol(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x01);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(rol_byte_by_4) {
    setup();
    state->regs[REG_AX] = 0x12;
    DecodeContext d = unary_byte(0);
    exec_rol(state, &d, 4);
    ASSERT_EQ(read_reg8(state, 0), 0x21);
    teardown();
}

TEST(rol_word_by_1) {
    setup();
    state->regs[REG_AX] = 0x8001;
    DecodeContext d = unary_word(REG_AX);
    exec_rol(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x0003);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(rol_does_not_affect_szp) {
    setup();
    set_flag(state, FLAG_ZF);
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_rol(state, &d, 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1); /* unchanged */
    teardown();
}

TEST(rol_count_zero_noop) {
    setup();
    state->regs[REG_AX] = 0x42;
    uint16_t flags_before = state->flags;
    DecodeContext d = unary_byte(0);
    exec_rol(state, &d, 0);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    ASSERT_EQ(state->flags, flags_before);
    teardown();
}

/* === ROR === */

TEST(ror_byte_by_1) {
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_ror(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(ror_byte_by_4) {
    setup();
    state->regs[REG_AX] = 0x12;
    DecodeContext d = unary_byte(0);
    exec_ror(state, &d, 4);
    ASSERT_EQ(read_reg8(state, 0), 0x21);
    teardown();
}

TEST(ror_word_by_1) {
    setup();
    state->regs[REG_AX] = 0x0001;
    DecodeContext d = unary_word(REG_AX);
    exec_ror(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x8000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(ror_does_not_affect_szp) {
    setup();
    set_flag(state, FLAG_ZF);
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_ror(state, &d, 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

/* === RCL === */

TEST(rcl_byte_by_1_cf_clear) {
    setup();
    clear_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_rcl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(rcl_byte_by_1_cf_set) {
    setup();
    set_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x00;
    DecodeContext d = unary_byte(0);
    exec_rcl(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x01);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(rcl_word_by_1) {
    setup();
    set_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x0000;
    DecodeContext d = unary_word(REG_AX);
    exec_rcl(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x0001);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(rcl_9bit_rotation) {
    setup();
    clear_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_rcl(state, &d, 9); /* 9 = full rotation for 8-bit + CF */
    ASSERT_EQ(read_reg8(state, 0), 0x01);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

/* === RCR === */

TEST(rcr_byte_by_1_cf_clear) {
    setup();
    clear_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_rcr(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(rcr_byte_by_1_cf_set) {
    setup();
    set_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x00;
    DecodeContext d = unary_byte(0);
    exec_rcr(state, &d, 1);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(rcr_word_by_1) {
    setup();
    set_flag(state, FLAG_CF);
    state->regs[REG_AX] = 0x0000;
    DecodeContext d = unary_word(REG_AX);
    exec_rcr(state, &d, 1);
    ASSERT_EQ(state->regs[REG_AX], 0x8000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

int main(void) {
    printf("test_shift:\n");
    /* SHL */
    RUN_TEST(shl_byte_by_1); RUN_TEST(shl_byte_by_1_no_carry);
    RUN_TEST(shl_byte_by_4); RUN_TEST(shl_word_by_1);
    RUN_TEST(shl_of_set); RUN_TEST(shl_of_clear);
    RUN_TEST(shl_sets_szp); RUN_TEST(shl_count_zero_noop);
    /* SHR */
    RUN_TEST(shr_byte_by_1); RUN_TEST(shr_byte_by_1_no_carry);
    RUN_TEST(shr_byte_by_4); RUN_TEST(shr_word_by_1);
    RUN_TEST(shr_of_set); RUN_TEST(shr_of_clear); RUN_TEST(shr_fills_zero);
    /* SAR */
    RUN_TEST(sar_byte_positive); RUN_TEST(sar_byte_negative);
    RUN_TEST(sar_byte_negative_by_4); RUN_TEST(sar_word_negative);
    RUN_TEST(sar_of_zero_for_count_1); RUN_TEST(sar_preserves_sign);
    /* ROL */
    RUN_TEST(rol_byte_by_1); RUN_TEST(rol_byte_by_4);
    RUN_TEST(rol_word_by_1); RUN_TEST(rol_does_not_affect_szp);
    RUN_TEST(rol_count_zero_noop);
    /* ROR */
    RUN_TEST(ror_byte_by_1); RUN_TEST(ror_byte_by_4);
    RUN_TEST(ror_word_by_1); RUN_TEST(ror_does_not_affect_szp);
    /* RCL */
    RUN_TEST(rcl_byte_by_1_cf_clear); RUN_TEST(rcl_byte_by_1_cf_set);
    RUN_TEST(rcl_word_by_1); RUN_TEST(rcl_9bit_rotation);
    /* RCR */
    RUN_TEST(rcr_byte_by_1_cf_clear); RUN_TEST(rcr_byte_by_1_cf_set);
    RUN_TEST(rcr_word_by_1);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
