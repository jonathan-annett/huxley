#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/tables.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/arithmetic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;

static void setup(void)
{
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!state) { fprintf(stderr, "alloc failed\n"); exit(1); }
    emu86_init(state);
}

static void teardown(void)
{
    free(state);
    state = NULL;
}

/*
 * Helper: set up a DecodeContext for a register-to-register operation.
 * direction=0 means dest=rm, src=reg.
 */
static DecodeContext make_ctx(uint8_t width, uint8_t rm_reg, uint8_t src_reg, uint8_t dir)
{
    DecodeContext d;
    memset(&d, 0, sizeof(d));
    d.mod = 3; /* register mode */
    d.rm = rm_reg;
    d.reg = src_reg;
    d.operand_width = width;
    d.direction = dir;
    return d;
}

/* Shorthand: byte op, dest=rm_reg, src=src_reg, direction=0 */
static DecodeContext byte_op(uint8_t rm_reg, uint8_t src_reg)
{
    return make_ctx(0, rm_reg, src_reg, 0);
}

/* Shorthand: word op, dest=rm_reg, src=src_reg, direction=0 */
static DecodeContext word_op(uint8_t rm_reg, uint8_t src_reg)
{
    return make_ctx(1, rm_reg, src_reg, 0);
}

/* Shorthand: unary op on rm_reg (INC, DEC, NEG, MUL, etc.) */
static DecodeContext unary_byte(uint8_t rm_reg)
{
    DecodeContext d;
    memset(&d, 0, sizeof(d));
    d.mod = 3;
    d.rm = rm_reg;
    d.operand_width = 0;
    return d;
}

static DecodeContext unary_word(uint8_t rm_reg)
{
    DecodeContext d;
    memset(&d, 0, sizeof(d));
    d.mod = 3;
    d.rm = rm_reg;
    d.operand_width = 1;
    return d;
}

/* ================================================================
 * Flag helper tests
 * ================================================================ */

TEST(parity8_test)
{
    ASSERT_EQ(parity8(0x00), 1); /* 0 bits set → even */
    ASSERT_EQ(parity8(0x01), 0); /* 1 bit → odd */
    ASSERT_EQ(parity8(0x03), 1); /* 2 bits → even */
    ASSERT_EQ(parity8(0xFF), 1); /* 8 bits → even */
    ASSERT_EQ(parity8(0x07), 0); /* 3 bits → odd */
}

TEST(set_flags_szp_byte)
{
    setup();
    set_flags_szp(state, 0x00, 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1);

    set_flags_szp(state, 0x80, 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    ASSERT_EQ(get_flag(state, FLAG_PF), 0); /* 0x80 = 1 bit → odd */

    set_flags_szp(state, 0x01, 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 0); /* 1 bit → odd */

    set_flags_szp(state, 0xFF, 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1); /* 8 bits → even */
    teardown();
}

TEST(set_flags_szp_word)
{
    setup();
    set_flags_szp(state, 0x0000, 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1);

    set_flags_szp(state, 0x8000, 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1); /* low byte 0x00 → even */

    set_flags_szp(state, 0x0100, 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1); /* low byte 0x00 → even */
    teardown();
}

/* ================================================================
 * ADD tests
 * ================================================================ */

TEST(add_byte_no_flags)
{
    setup();
    state->regs[REG_AX] = 0x10; /* AL=0x10 */
    state->regs[REG_BX] = 0x20; /* BL=0x20 */
    DecodeContext d = byte_op(0, 3); /* AL += BL */
    exec_add(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x30);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(add_byte_carry)
{
    setup();
    state->regs[REG_AX] = 0xFF; /* AL=0xFF */
    state->regs[REG_BX] = 0x01; /* BL=0x01 */
    DecodeContext d = byte_op(0, 3);
    exec_add(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(add_byte_overflow)
{
    setup();
    state->regs[REG_AX] = 0x7F;
    state->regs[REG_BX] = 0x01;
    DecodeContext d = byte_op(0, 3);
    exec_add(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    teardown();
}

TEST(add_byte_auxiliary_carry)
{
    setup();
    state->regs[REG_AX] = 0x0F;
    state->regs[REG_BX] = 0x01;
    DecodeContext d = byte_op(0, 3);
    exec_add(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_AF), 1);
    teardown();
}

TEST(add_word_basic)
{
    setup();
    state->regs[REG_AX] = 0x1000;
    state->regs[REG_BX] = 0x2000;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_add(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x3000);
    teardown();
}

TEST(add_word_carry)
{
    setup();
    state->regs[REG_AX] = 0xFFFF;
    state->regs[REG_BX] = 0x0001;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_add(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(add_word_overflow)
{
    setup();
    state->regs[REG_AX] = 0x7FFF;
    state->regs[REG_BX] = 0x0001;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_add(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x8000);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

/* ================================================================
 * ADC tests
 * ================================================================ */

TEST(adc_without_carry)
{
    setup();
    state->regs[REG_AX] = 0x10;
    state->regs[REG_BX] = 0x20;
    clear_flag(state, FLAG_CF);
    DecodeContext d = byte_op(0, 3);
    exec_adc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x30);
    teardown();
}

TEST(adc_with_carry)
{
    setup();
    state->regs[REG_AX] = 0x10;
    state->regs[REG_BX] = 0x20;
    set_flag(state, FLAG_CF);
    DecodeContext d = byte_op(0, 3);
    exec_adc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x31);
    teardown();
}

TEST(adc_carry_propagation)
{
    setup();
    state->regs[REG_AX] = 0xFF;
    state->regs[REG_BX] = 0x00;
    set_flag(state, FLAG_CF);
    DecodeContext d = byte_op(0, 3);
    exec_adc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

/* ================================================================
 * SUB tests
 * ================================================================ */

TEST(sub_byte_no_borrow)
{
    setup();
    state->regs[REG_AX] = 0x30;
    state->regs[REG_BX] = 0x10;
    DecodeContext d = byte_op(0, 3);
    exec_sub(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x20);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}

TEST(sub_byte_borrow)
{
    setup();
    state->regs[REG_AX] = 0x00;
    state->regs[REG_BX] = 0x01;
    DecodeContext d = byte_op(0, 3);
    exec_sub(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(sub_byte_overflow)
{
    setup();
    state->regs[REG_AX] = 0x80; /* -128 */
    state->regs[REG_BX] = 0x01;
    DecodeContext d = byte_op(0, 3);
    exec_sub(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x7F);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(sub_word_basic)
{
    setup();
    state->regs[REG_AX] = 0x3000;
    state->regs[REG_BX] = 0x1000;
    DecodeContext d = word_op(REG_AX, REG_BX);
    exec_sub(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x2000);
    teardown();
}

/* ================================================================
 * SBB tests
 * ================================================================ */

TEST(sbb_without_borrow)
{
    setup();
    state->regs[REG_AX] = 0x30;
    state->regs[REG_BX] = 0x10;
    clear_flag(state, FLAG_CF);
    DecodeContext d = byte_op(0, 3);
    exec_sbb(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x20);
    teardown();
}

TEST(sbb_with_borrow)
{
    setup();
    state->regs[REG_AX] = 0x30;
    state->regs[REG_BX] = 0x10;
    set_flag(state, FLAG_CF);
    DecodeContext d = byte_op(0, 3);
    exec_sbb(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x1F);
    teardown();
}

/* ================================================================
 * CMP tests
 * ================================================================ */

TEST(cmp_equal)
{
    setup();
    state->regs[REG_AX] = 0x42;
    state->regs[REG_BX] = 0x42;
    DecodeContext d = byte_op(0, 3);
    exec_cmp(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(read_reg8(state, 0), 0x42); /* AL unchanged */
    teardown();
}

TEST(cmp_less)
{
    setup();
    state->regs[REG_AX] = 0x10;
    state->regs[REG_BX] = 0x20;
    DecodeContext d = byte_op(0, 3);
    exec_cmp(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(read_reg8(state, 0), 0x10); /* AL unchanged */
    teardown();
}

TEST(cmp_greater)
{
    setup();
    state->regs[REG_AX] = 0x20;
    state->regs[REG_BX] = 0x10;
    DecodeContext d = byte_op(0, 3);
    exec_cmp(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    ASSERT_EQ(read_reg8(state, 0), 0x20); /* AL unchanged */
    teardown();
}

/* ================================================================
 * NEG tests
 * ================================================================ */

TEST(neg_positive)
{
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0); /* AL */
    exec_neg(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0xFF);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(neg_zero)
{
    setup();
    state->regs[REG_AX] = 0x00;
    DecodeContext d = unary_byte(0);
    exec_neg(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(neg_overflow)
{
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_neg(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(neg_word)
{
    setup();
    state->regs[REG_AX] = 0x0001;
    DecodeContext d = unary_word(REG_AX);
    exec_neg(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0xFFFF);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

/* ================================================================
 * INC / DEC tests
 * ================================================================ */

TEST(inc_basic)
{
    setup();
    state->regs[REG_AX] = 0x41;
    DecodeContext d = unary_byte(0);
    exec_inc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    teardown();
}

TEST(inc_does_not_affect_carry)
{
    setup();
    state->regs[REG_AX] = 0x41;
    set_flag(state, FLAG_CF);
    DecodeContext d = unary_byte(0);
    exec_inc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1); /* CF preserved */
    teardown();
}

TEST(inc_overflow)
{
    setup();
    state->regs[REG_AX] = 0x7F;
    DecodeContext d = unary_byte(0);
    exec_inc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x80);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(inc_byte_wrap)
{
    setup();
    state->regs[REG_AX] = 0xFF;
    set_flag(state, FLAG_CF); /* set CF before to verify it's not cleared */
    DecodeContext d = unary_byte(0);
    exec_inc(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1); /* CF unchanged! */
    teardown();
}

TEST(dec_basic)
{
    setup();
    state->regs[REG_AX] = 0x42;
    DecodeContext d = unary_byte(0);
    exec_dec(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x41);
    teardown();
}

TEST(dec_does_not_affect_carry)
{
    setup();
    state->regs[REG_AX] = 0x42;
    set_flag(state, FLAG_CF);
    DecodeContext d = unary_byte(0);
    exec_dec(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x41);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(dec_overflow)
{
    setup();
    state->regs[REG_AX] = 0x80;
    DecodeContext d = unary_byte(0);
    exec_dec(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x7F);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(dec_zero)
{
    setup();
    state->regs[REG_AX] = 0x01;
    DecodeContext d = unary_byte(0);
    exec_dec(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}

TEST(inc_word)
{
    setup();
    state->regs[REG_AX] = 0xFFFF;
    clear_flag(state, FLAG_CF);
    DecodeContext d = unary_word(REG_AX);
    exec_inc(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0); /* CF unchanged (was 0) */
    teardown();
}

/* ================================================================
 * MUL tests
 * ================================================================ */

TEST(mul_byte_basic)
{
    setup();
    state->regs[REG_AX] = 3;  /* AL = 3 */
    state->regs[REG_CX] = 4;  /* CL = 4 */
    DecodeContext d = unary_byte(1); /* r/m = CL */
    exec_mul(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 12);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(mul_byte_high_result)
{
    setup();
    state->regs[REG_AX] = 0x80; /* AL = 128 */
    state->regs[REG_CX] = 0x02; /* CL = 2 */
    DecodeContext d = unary_byte(1);
    exec_mul(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0100);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(mul_word_basic)
{
    setup();
    state->regs[REG_AX] = 100;
    state->regs[REG_CX] = 200;
    DecodeContext d = unary_word(REG_CX);
    exec_mul(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 20000);
    ASSERT_EQ(state->regs[REG_DX], 0);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(mul_word_high_result)
{
    setup();
    state->regs[REG_AX] = 0xFFFF;
    state->regs[REG_CX] = 0xFFFF;
    DecodeContext d = unary_word(REG_CX);
    exec_mul(state, &d);
    /* 0xFFFF * 0xFFFF = 0xFFFE0001 */
    ASSERT_EQ(state->regs[REG_AX], 0x0001);
    ASSERT_EQ(state->regs[REG_DX], 0xFFFE);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

/* ================================================================
 * IMUL tests
 * ================================================================ */

TEST(imul_byte_basic)
{
    setup();
    state->regs[REG_AX] = 3;
    state->regs[REG_CX] = 4;
    DecodeContext d = unary_byte(1);
    exec_imul(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 12);
    teardown();
}

TEST(imul_byte_negative)
{
    setup();
    state->regs[REG_AX] = 0xFE; /* AL = -2 */
    state->regs[REG_CX] = 3;
    DecodeContext d = unary_byte(1);
    exec_imul(state, &d);
    /* -2 * 3 = -6 = 0xFFFA. -6 fits in signed byte, so CF=OF=0 */
    ASSERT_EQ(state->regs[REG_AX], 0xFFFA);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);

    /* Now test a case that truly doesn't fit: AL=-128, operand=2 → -256 */
    state->regs[REG_AX] = 0x80; /* AL = -128 */
    state->regs[REG_CX] = 2;
    exec_imul(state, &d);
    /* -128 * 2 = -256 = 0xFF00. Doesn't fit in signed byte [-128,127] */
    ASSERT_EQ(state->regs[REG_AX], 0xFF00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(imul_byte_fits)
{
    setup();
    state->regs[REG_AX] = 0xFF; /* AL = -1 */
    state->regs[REG_CX] = 1;
    DecodeContext d = unary_byte(1);
    exec_imul(state, &d);
    /* -1 * 1 = -1 = 0xFFFF */
    ASSERT_EQ(state->regs[REG_AX], 0xFFFF);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0); /* -1 fits in signed byte */
    ASSERT_EQ(get_flag(state, FLAG_OF), 0);
    teardown();
}

TEST(imul_word_basic)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 20;
    DecodeContext d = unary_word(REG_CX);
    exec_imul(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 200);
    ASSERT_EQ(state->regs[REG_DX], 0);
    teardown();
}

/* ================================================================
 * DIV tests
 * ================================================================ */

TEST(div_byte_basic)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 3;
    DecodeContext d = unary_byte(1);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(read_reg8(state, 0), 3);  /* AL = quotient */
    ASSERT_EQ(read_reg8(state, 4), 1);  /* AH = remainder */
    teardown();
}

TEST(div_byte_exact)
{
    setup();
    state->regs[REG_AX] = 12;
    state->regs[REG_CX] = 4;
    DecodeContext d = unary_byte(1);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(read_reg8(state, 0), 3);  /* AL = 3 */
    ASSERT_EQ(read_reg8(state, 4), 0);  /* AH = 0 */
    teardown();
}

TEST(div_byte_by_zero)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 0;
    DecodeContext d = unary_byte(1);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 1); /* divide by zero */
    teardown();
}

TEST(div_byte_overflow)
{
    setup();
    state->regs[REG_AX] = 0x0400; /* 1024 */
    state->regs[REG_CX] = 1;
    DecodeContext d = unary_byte(1);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 1); /* quotient 1024 > 255 */
    teardown();
}

TEST(div_word_basic)
{
    setup();
    state->regs[REG_DX] = 0;
    state->regs[REG_AX] = 1000;
    state->regs[REG_CX] = 7;
    DecodeContext d = unary_word(REG_CX);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(state->regs[REG_AX], 142);
    ASSERT_EQ(state->regs[REG_DX], 6);
    teardown();
}

TEST(div_word_by_zero)
{
    setup();
    state->regs[REG_DX] = 0;
    state->regs[REG_AX] = 1000;
    state->regs[REG_CX] = 0;
    DecodeContext d = unary_word(REG_CX);
    int err = exec_div(state, &d);
    ASSERT_EQ(err, 1);
    teardown();
}

/* ================================================================
 * IDIV tests
 * ================================================================ */

TEST(idiv_byte_basic)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 3;
    DecodeContext d = unary_byte(1);
    int err = exec_idiv(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(read_reg8(state, 0), 3);  /* AL = quotient */
    ASSERT_EQ(read_reg8(state, 4), 1);  /* AH = remainder */
    teardown();
}

TEST(idiv_byte_negative_dividend)
{
    setup();
    state->regs[REG_AX] = 0xFFF6; /* -10 as int16_t */
    state->regs[REG_CX] = 3;
    DecodeContext d = unary_byte(1);
    int err = exec_idiv(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(read_reg8(state, 0), 0xFD); /* AL = -3 */
    ASSERT_EQ(read_reg8(state, 4), 0xFF); /* AH = -1 */
    teardown();
}

TEST(idiv_byte_negative_divisor)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 0xFD; /* CL = -3 */
    DecodeContext d = unary_byte(1);
    int err = exec_idiv(state, &d);
    ASSERT_EQ(err, 0);
    ASSERT_EQ(read_reg8(state, 0), 0xFD); /* AL = -3 */
    ASSERT_EQ(read_reg8(state, 4), 1);    /* AH = 1 */
    teardown();
}

TEST(idiv_byte_by_zero)
{
    setup();
    state->regs[REG_AX] = 10;
    state->regs[REG_CX] = 0;
    DecodeContext d = unary_byte(1);
    int err = exec_idiv(state, &d);
    ASSERT_EQ(err, 1);
    teardown();
}

/* ================================================================
 * BCD tests
 * ================================================================ */

TEST(daa_basic)
{
    setup();
    /* Simulate: AL = 0x0A (result of 5+5, lower nibble > 9) */
    state->regs[REG_AX] = 0x0A;
    clear_flag(state, FLAG_AF);
    clear_flag(state, FLAG_CF);
    exec_daa(state);
    ASSERT_EQ(read_reg8(state, 0), 0x10);
    ASSERT_EQ(get_flag(state, FLAG_AF), 1);
    teardown();
}

TEST(daa_both_nibbles)
{
    setup();
    state->regs[REG_AX] = 0x9A;
    clear_flag(state, FLAG_AF);
    clear_flag(state, FLAG_CF);
    exec_daa(state);
    ASSERT_EQ(read_reg8(state, 0), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(das_basic)
{
    setup();
    /* AL=0x10 with AF set (as if 0x16 - 0x06 produced 0x10 with AF) */
    state->regs[REG_AX] = 0x10;
    set_flag(state, FLAG_AF);
    clear_flag(state, FLAG_CF);
    exec_das(state);
    ASSERT_EQ(read_reg8(state, 0), 0x0A);
    teardown();
}

TEST(aaa_basic)
{
    setup();
    state->regs[REG_AX] = 0x000A; /* AH=0, AL=0x0A */
    clear_flag(state, FLAG_AF);
    exec_aaa(state);
    ASSERT_EQ(read_reg8(state, 0), 0x00); /* AL low nibble = 0 */
    ASSERT_EQ(read_reg8(state, 4), 0x01); /* AH incremented */
    ASSERT_EQ(get_flag(state, FLAG_AF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(aas_basic)
{
    setup();
    /* AH=1, AL=0xFF (e.g., subtraction underflow) */
    state->regs[REG_AX] = 0x01FF;
    set_flag(state, FLAG_AF);
    exec_aas(state);
    /* AL = (0xFF - 6) & 0x0F = 0xF9 & 0x0F = 0x09, AH = 1 - 1 = 0 */
    ASSERT_EQ(read_reg8(state, 0), 0x09);
    ASSERT_EQ(read_reg8(state, 4), 0x00);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}

TEST(aam_basic)
{
    setup();
    state->regs[REG_AX] = 15; /* AL = 15 */
    exec_aam(state, 10);
    ASSERT_EQ(read_reg8(state, 4), 1); /* AH = 15/10 = 1 */
    ASSERT_EQ(read_reg8(state, 0), 5); /* AL = 15%10 = 5 */
    teardown();
}

TEST(aad_basic)
{
    setup();
    state->regs[REG_AX] = 0x0105; /* AH=1, AL=5 */
    exec_aad(state, 10);
    ASSERT_EQ(read_reg8(state, 0), 15); /* 1*10 + 5 */
    ASSERT_EQ(read_reg8(state, 4), 0);  /* AH = 0 */
    teardown();
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    printf("test_arithmetic:\n");

    /* Flag helpers */
    RUN_TEST(parity8_test);
    RUN_TEST(set_flags_szp_byte);
    RUN_TEST(set_flags_szp_word);

    /* ADD */
    RUN_TEST(add_byte_no_flags);
    RUN_TEST(add_byte_carry);
    RUN_TEST(add_byte_overflow);
    RUN_TEST(add_byte_auxiliary_carry);
    RUN_TEST(add_word_basic);
    RUN_TEST(add_word_carry);
    RUN_TEST(add_word_overflow);

    /* ADC */
    RUN_TEST(adc_without_carry);
    RUN_TEST(adc_with_carry);
    RUN_TEST(adc_carry_propagation);

    /* SUB */
    RUN_TEST(sub_byte_no_borrow);
    RUN_TEST(sub_byte_borrow);
    RUN_TEST(sub_byte_overflow);
    RUN_TEST(sub_word_basic);

    /* SBB */
    RUN_TEST(sbb_without_borrow);
    RUN_TEST(sbb_with_borrow);

    /* CMP */
    RUN_TEST(cmp_equal);
    RUN_TEST(cmp_less);
    RUN_TEST(cmp_greater);

    /* NEG */
    RUN_TEST(neg_positive);
    RUN_TEST(neg_zero);
    RUN_TEST(neg_overflow);
    RUN_TEST(neg_word);

    /* INC / DEC */
    RUN_TEST(inc_basic);
    RUN_TEST(inc_does_not_affect_carry);
    RUN_TEST(inc_overflow);
    RUN_TEST(inc_byte_wrap);
    RUN_TEST(dec_basic);
    RUN_TEST(dec_does_not_affect_carry);
    RUN_TEST(dec_overflow);
    RUN_TEST(dec_zero);
    RUN_TEST(inc_word);

    /* MUL */
    RUN_TEST(mul_byte_basic);
    RUN_TEST(mul_byte_high_result);
    RUN_TEST(mul_word_basic);
    RUN_TEST(mul_word_high_result);

    /* IMUL */
    RUN_TEST(imul_byte_basic);
    RUN_TEST(imul_byte_negative);
    RUN_TEST(imul_byte_fits);
    RUN_TEST(imul_word_basic);

    /* DIV */
    RUN_TEST(div_byte_basic);
    RUN_TEST(div_byte_exact);
    RUN_TEST(div_byte_by_zero);
    RUN_TEST(div_byte_overflow);
    RUN_TEST(div_word_basic);
    RUN_TEST(div_word_by_zero);

    /* IDIV */
    RUN_TEST(idiv_byte_basic);
    RUN_TEST(idiv_byte_negative_dividend);
    RUN_TEST(idiv_byte_negative_divisor);
    RUN_TEST(idiv_byte_by_zero);

    /* BCD */
    RUN_TEST(daa_basic);
    RUN_TEST(daa_both_nibbles);
    RUN_TEST(das_basic);
    RUN_TEST(aaa_basic);
    RUN_TEST(aas_basic);
    RUN_TEST(aam_basic);
    RUN_TEST(aad_basic);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
