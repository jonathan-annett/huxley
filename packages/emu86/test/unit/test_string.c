#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/string.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);
    state->sregs[SREG_DS] = 0x0000;
    state->sregs[SREG_ES] = 0x0000;
}
static void teardown(void) { free(state); state = NULL; }

/* === Index advancement === */
TEST(index_advance_forward_byte) {
    setup(); state->regs[REG_SI] = 0x100; clear_flag(state, FLAG_DF);
    index_advance(state, REG_SI, 0);
    ASSERT_EQ(state->regs[REG_SI], 0x101); teardown();
}
TEST(index_advance_forward_word) {
    setup(); state->regs[REG_SI] = 0x100; clear_flag(state, FLAG_DF);
    index_advance(state, REG_SI, 1);
    ASSERT_EQ(state->regs[REG_SI], 0x102); teardown();
}
TEST(index_advance_backward_byte) {
    setup(); state->regs[REG_SI] = 0x100; set_flag(state, FLAG_DF);
    index_advance(state, REG_SI, 0);
    ASSERT_EQ(state->regs[REG_SI], 0x0FF); teardown();
}
TEST(index_advance_backward_word) {
    setup(); state->regs[REG_SI] = 0x100; set_flag(state, FLAG_DF);
    index_advance(state, REG_SI, 1);
    ASSERT_EQ(state->regs[REG_SI], 0x0FE); teardown();
}

/* === MOVSB/MOVSW === */
TEST(movsb_single) {
    setup(); mem_write8(state, 0x200, 0x42);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_movsb(state);
    ASSERT_EQ(mem_read8(state, 0x300), 0x42);
    ASSERT_EQ(state->regs[REG_SI], 0x201); ASSERT_EQ(state->regs[REG_DI], 0x301);
    teardown();
}
TEST(movsw_single) {
    setup(); mem_write16(state, 0x200, 0x1234);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_movsw(state);
    ASSERT_EQ(mem_read16(state, 0x300), 0x1234);
    ASSERT_EQ(state->regs[REG_SI], 0x202); ASSERT_EQ(state->regs[REG_DI], 0x302);
    teardown();
}
TEST(movsb_backward) {
    setup(); set_flag(state, FLAG_DF);
    mem_write8(state, 0x200, 0x99);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_movsb(state);
    ASSERT_EQ(state->regs[REG_SI], 0x1FF); ASSERT_EQ(state->regs[REG_DI], 0x2FF);
    teardown();
}
TEST(movsb_no_flags) {
    setup(); set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    mem_write8(state, 0x200, 0x42);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_movsb(state);
    ASSERT_EQ(state->flags, before); teardown();
}
TEST(rep_movsb_block_copy) {
    setup();
    for (int i = 0; i < 10; i++) mem_write8(state, 0x200 + i, (uint8_t)(0xA0 + i));
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x400;
    state->regs[REG_CX] = 10; state->rep_override_en = 2; state->rep_mode = 1;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA4; d.operand_width = 0;
    exec_string_op(state, &d);
    for (int i = 0; i < 10; i++) ASSERT_EQ(mem_read8(state, 0x400 + i), (uint8_t)(0xA0 + i));
    ASSERT_EQ(state->regs[REG_CX], 0);
    ASSERT_EQ(state->regs[REG_SI], 0x20A); ASSERT_EQ(state->regs[REG_DI], 0x40A);
    teardown();
}
TEST(rep_movsb_cx_zero) {
    setup();
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x400;
    state->regs[REG_CX] = 0; state->rep_override_en = 2;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA4; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_SI], 0x200); ASSERT_EQ(state->regs[REG_DI], 0x400);
    teardown();
}
TEST(rep_movsw_block_copy) {
    setup();
    for (int i = 0; i < 5; i++) mem_write16(state, 0x200 + i*2, (uint16_t)(0x1000 + i));
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x400;
    state->regs[REG_CX] = 5; state->rep_override_en = 2;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA5; d.operand_width = 1;
    exec_string_op(state, &d);
    for (int i = 0; i < 5; i++) ASSERT_EQ(mem_read16(state, 0x400 + i*2), (uint16_t)(0x1000 + i));
    ASSERT_EQ(state->regs[REG_CX], 0); teardown();
}

/* === CMPSB === */
TEST(cmpsb_equal) {
    setup(); mem_write8(state, 0x200, 0x42); mem_write8(state, 0x300, 0x42);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_cmpsb(state);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1); ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}
TEST(cmpsb_less) {
    setup(); mem_write8(state, 0x200, 0x10); mem_write8(state, 0x300, 0x20);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_cmpsb(state);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0); ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    teardown();
}
TEST(cmpsb_greater) {
    setup(); mem_write8(state, 0x200, 0x20); mem_write8(state, 0x300, 0x10);
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    exec_cmpsb(state);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0); ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    teardown();
}
TEST(repz_cmpsb_find_difference) {
    setup();
    const uint8_t s1[] = {'A','B','C','D','E'};
    const uint8_t s2[] = {'A','B','C','X','E'};
    for (int i = 0; i < 5; i++) { mem_write8(state, 0x200+i, s1[i]); mem_write8(state, 0x300+i, s2[i]); }
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    state->regs[REG_CX] = 5; state->rep_override_en = 2; state->rep_mode = 1;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA6; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 1); /* 5 - 4 iterations */
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0); teardown();
}
TEST(repz_cmpsb_all_equal) {
    setup();
    for (int i = 0; i < 4; i++) { mem_write8(state, 0x200+i, 'A'); mem_write8(state, 0x300+i, 'A'); }
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    state->regs[REG_CX] = 4; state->rep_override_en = 2; state->rep_mode = 1;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA6; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 0); ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}
TEST(repnz_cmpsb_find_match) {
    setup();
    const uint8_t s1[] = {'A','B','C','D'};
    const uint8_t s2[] = {'X','X','C','X'};
    for (int i = 0; i < 4; i++) { mem_write8(state, 0x200+i, s1[i]); mem_write8(state, 0x300+i, s2[i]); }
    state->regs[REG_SI] = 0x200; state->regs[REG_DI] = 0x300;
    state->regs[REG_CX] = 4; state->rep_override_en = 2; state->rep_mode = 0;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xA6; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1); teardown();
}

/* === STOSB/STOSW === */
TEST(stosb_single) {
    setup(); state->regs[REG_AX] = 0x42; state->regs[REG_DI] = 0x300;
    exec_stosb(state);
    ASSERT_EQ(mem_read8(state, 0x300), 0x42); ASSERT_EQ(state->regs[REG_DI], 0x301);
    teardown();
}
TEST(stosw_single) {
    setup(); state->regs[REG_AX] = 0x1234; state->regs[REG_DI] = 0x300;
    exec_stosw(state);
    ASSERT_EQ(mem_read16(state, 0x300), 0x1234); ASSERT_EQ(state->regs[REG_DI], 0x302);
    teardown();
}
TEST(rep_stosb_fill) {
    setup(); state->regs[REG_AX] = 0xFF; state->regs[REG_DI] = 0x400;
    state->regs[REG_CX] = 16; state->rep_override_en = 2;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xAA; d.operand_width = 0;
    exec_string_op(state, &d);
    for (int i = 0; i < 16; i++) ASSERT_EQ(mem_read8(state, 0x400 + i), 0xFF);
    ASSERT_EQ(state->regs[REG_CX], 0); teardown();
}
TEST(stosb_no_flags) {
    setup(); set_flag(state, FLAG_CF); uint16_t before = state->flags;
    state->regs[REG_AX] = 0x42; state->regs[REG_DI] = 0x300;
    exec_stosb(state);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === LODSB/LODSW === */
TEST(lodsb_single) {
    setup(); mem_write8(state, 0x200, 0x42); state->regs[REG_SI] = 0x200;
    exec_lodsb(state);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x42); ASSERT_EQ(state->regs[REG_SI], 0x201);
    teardown();
}
TEST(lodsw_single) {
    setup(); mem_write16(state, 0x200, 0x1234); state->regs[REG_SI] = 0x200;
    exec_lodsw(state);
    ASSERT_EQ(state->regs[REG_AX], 0x1234); ASSERT_EQ(state->regs[REG_SI], 0x202);
    teardown();
}
TEST(lodsb_no_flags) {
    setup(); set_flag(state, FLAG_CF); uint16_t before = state->flags;
    mem_write8(state, 0x200, 0x42); state->regs[REG_SI] = 0x200;
    exec_lodsb(state);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === SCASB/SCASW === */
TEST(scasb_match) {
    setup(); state->regs[REG_AX] = 0x42; mem_write8(state, 0x300, 0x42);
    state->regs[REG_DI] = 0x300;
    exec_scasb(state);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1); teardown();
}
TEST(scasb_no_match) {
    setup(); state->regs[REG_AX] = 0x42; mem_write8(state, 0x300, 0x99);
    state->regs[REG_DI] = 0x300;
    exec_scasb(state);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0); teardown();
}
TEST(repnz_scasb_find_byte) {
    setup(); state->regs[REG_AX] = 0x42;
    uint8_t data[] = {0x00, 0x00, 0x42, 0x00};
    for (int i = 0; i < 4; i++) mem_write8(state, 0x400 + i, data[i]);
    state->regs[REG_DI] = 0x400; state->regs[REG_CX] = 4;
    state->rep_override_en = 2; state->rep_mode = 0;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xAE; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 1); ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    teardown();
}
TEST(repnz_scasb_not_found) {
    setup(); state->regs[REG_AX] = 0xFF;
    for (int i = 0; i < 4; i++) mem_write8(state, 0x400 + i, 0x00);
    state->regs[REG_DI] = 0x400; state->regs[REG_CX] = 4;
    state->rep_override_en = 2; state->rep_mode = 0;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xAE; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 0); ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    teardown();
}
TEST(repz_scasb_scan_while_equal) {
    setup(); state->regs[REG_AX] = 0x42;
    uint8_t data[] = {0x42, 0x42, 0x42, 0x99};
    for (int i = 0; i < 4; i++) mem_write8(state, 0x400 + i, data[i]);
    state->regs[REG_DI] = 0x400; state->regs[REG_CX] = 4;
    state->rep_override_en = 2; state->rep_mode = 1;
    DecodeContext d; memset(&d, 0, sizeof(d)); d.opcode = 0xAE; d.operand_width = 0;
    exec_string_op(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 0); /* all 4 iterations run, last one sets ZF=0 */
    ASSERT_EQ(get_flag(state, FLAG_ZF), 0);
    teardown();
}

/* === Segment override === */
TEST(movsb_with_segment_override) {
    setup();
    state->sregs[SREG_ES] = 0x0100; /* ES segment */
    state->sregs[SREG_DS] = 0x0200; /* DS segment */
    /* Write source at ES:SI (overridden source) */
    mem_write8(state, segoff_to_linear(0x0100, 0x050), 0xAB);
    /* Normal DS:SI would point elsewhere */
    state->regs[REG_SI] = 0x050; state->regs[REG_DI] = 0x300;
    state->seg_override_en = 2; state->seg_override = SREG_ES;
    exec_movsb(state);
    /* Destination is always ES:DI */
    ASSERT_EQ(mem_read8(state, segoff_to_linear(0x0100, 0x300)), 0xAB);
    teardown();
}

int main(void) {
    printf("test_string:\n");
    RUN_TEST(index_advance_forward_byte); RUN_TEST(index_advance_forward_word);
    RUN_TEST(index_advance_backward_byte); RUN_TEST(index_advance_backward_word);
    RUN_TEST(movsb_single); RUN_TEST(movsw_single); RUN_TEST(movsb_backward);
    RUN_TEST(movsb_no_flags);
    RUN_TEST(rep_movsb_block_copy); RUN_TEST(rep_movsb_cx_zero); RUN_TEST(rep_movsw_block_copy);
    RUN_TEST(cmpsb_equal); RUN_TEST(cmpsb_less); RUN_TEST(cmpsb_greater);
    RUN_TEST(repz_cmpsb_find_difference); RUN_TEST(repz_cmpsb_all_equal);
    RUN_TEST(repnz_cmpsb_find_match);
    RUN_TEST(stosb_single); RUN_TEST(stosw_single);
    RUN_TEST(rep_stosb_fill); RUN_TEST(stosb_no_flags);
    RUN_TEST(lodsb_single); RUN_TEST(lodsw_single); RUN_TEST(lodsb_no_flags);
    RUN_TEST(scasb_match); RUN_TEST(scasb_no_match);
    RUN_TEST(repnz_scasb_find_byte); RUN_TEST(repnz_scasb_not_found);
    RUN_TEST(repz_scasb_scan_while_equal);
    RUN_TEST(movsb_with_segment_override);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
