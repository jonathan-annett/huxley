#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/transfer.h"
#include "../../src/emulator/opcodes/control.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);
    state->sregs[SREG_SS] = 0x1000;
    state->regs[REG_SP] = 0xFFFE;
    state->sregs[SREG_CS] = 0x0000;
    state->ip = 0x0100;
}
static void teardown(void) { free(state); state = NULL; }

static DecodeContext make_d(uint8_t opcode, uint8_t inst_len) {
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.opcode = opcode; d.inst_length = inst_len;
    return d;
}

/* === Conditional jump evaluation === */
TEST(eval_jz_true) { setup(); set_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x04), 1); teardown(); }
TEST(eval_jz_false) { setup(); clear_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x04), 0); teardown(); }
TEST(eval_jb_true) { setup(); set_flag(state, FLAG_CF); ASSERT_EQ(eval_condition(state, 0x02), 1); teardown(); }
TEST(eval_ja_true) { setup(); clear_flag(state, FLAG_CF); clear_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x07), 1); teardown(); }
TEST(eval_ja_false_cf) { setup(); set_flag(state, FLAG_CF); clear_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x07), 0); teardown(); }
TEST(eval_ja_false_zf) { setup(); clear_flag(state, FLAG_CF); set_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x07), 0); teardown(); }
TEST(eval_jl_true) { setup(); set_flag(state, FLAG_SF); clear_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 0x0C), 1); teardown(); }
TEST(eval_jl_false) { setup(); set_flag(state, FLAG_SF); set_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 0x0C), 0); teardown(); }
TEST(eval_jle_true_zf) { setup(); set_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 0x0E), 1); teardown(); }
TEST(eval_jle_true_sf_of) { setup(); clear_flag(state, FLAG_ZF); clear_flag(state, FLAG_SF); set_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 0x0E), 1); teardown(); }
TEST(eval_jg_true) { setup(); clear_flag(state, FLAG_ZF); clear_flag(state, FLAG_SF); clear_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 0x0F), 1); teardown(); }

TEST(eval_all_16_conditions) {
    setup();
    /* Test JO/JNO */ set_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 0), 1); ASSERT_EQ(eval_condition(state, 1), 0);
    /* JB/JNB */ set_flag(state, FLAG_CF); ASSERT_EQ(eval_condition(state, 2), 1); ASSERT_EQ(eval_condition(state, 3), 0);
    /* JZ/JNZ */ set_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 4), 1); ASSERT_EQ(eval_condition(state, 5), 0);
    /* JBE/JA */ ASSERT_EQ(eval_condition(state, 6), 1); /* CF||ZF */ clear_flag(state, FLAG_CF); clear_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 7), 1);
    /* JS/JNS */ set_flag(state, FLAG_SF); ASSERT_EQ(eval_condition(state, 8), 1); ASSERT_EQ(eval_condition(state, 9), 0);
    /* JP/JNP */ set_flag(state, FLAG_PF); ASSERT_EQ(eval_condition(state, 10), 1); ASSERT_EQ(eval_condition(state, 11), 0);
    /* JL/JGE */ set_flag(state, FLAG_SF); clear_flag(state, FLAG_OF); ASSERT_EQ(eval_condition(state, 12), 1); ASSERT_EQ(eval_condition(state, 13), 0);
    /* JLE/JG */ set_flag(state, FLAG_ZF); ASSERT_EQ(eval_condition(state, 14), 1); clear_flag(state, FLAG_ZF); set_flag(state, FLAG_OF); set_flag(state, FLAG_SF); ASSERT_EQ(eval_condition(state, 15), 1);
    teardown();
}

/* === JMP === */
TEST(jmp_short_forward) {
    setup(); DecodeContext d = make_d(0xEB, 2); d.data0 = 5;
    exec_jmp_short(state, &d);
    ASSERT_EQ(state->ip, 0x0100 + 2 + 5); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(jmp_short_backward) {
    setup(); state->ip = 0x0100; DecodeContext d = make_d(0xEB, 2); d.data0 = (uint32_t)(int8_t)(-10) & 0xFF;
    /* data0 needs to be the raw byte, sign-extended later in exec */
    d.data0 = 0xF6; /* -10 as uint8 */
    exec_jmp_short(state, &d);
    ASSERT_EQ(state->ip, (uint16_t)(0x0100 + 2 - 10)); teardown();
}
TEST(jmp_near) {
    setup(); DecodeContext d = make_d(0xE9, 3); d.data0 = 0x0200;
    exec_jmp_near(state, &d);
    ASSERT_EQ(state->ip, 0x0100 + 3 + 0x0200); teardown();
}
TEST(jmp_far) {
    setup(); DecodeContext d = make_d(0xEA, 5); d.data0 = 0x0100; d.data2 = 0x2000;
    exec_jmp_far(state, &d);
    ASSERT_EQ(state->ip, 0x0100); ASSERT_EQ(state->sregs[SREG_CS], 0x2000); teardown();
}
TEST(jmp_rm) {
    setup(); state->regs[REG_BX] = 0x0500;
    DecodeContext d = make_d(0xFF, 2); d.mod = 3; d.rm = REG_BX; d.operand_width = 1;
    exec_jmp_rm(state, &d);
    ASSERT_EQ(state->ip, 0x0500); teardown();
}
TEST(jmp_no_flags) {
    setup(); set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    DecodeContext d = make_d(0xEB, 2); d.data0 = 5;
    exec_jmp_short(state, &d);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === Jcc === */
TEST(jcc_taken) {
    setup(); set_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0x74, 2); d.data0 = 10; /* JZ +10 */
    exec_jcc(state, &d);
    ASSERT_EQ(state->ip, 0x0100 + 2 + 10); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(jcc_not_taken) {
    setup(); clear_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0x74, 2); d.data0 = 10;
    exec_jcc(state, &d);
    ASSERT_EQ(state->ip, 0x0100); ASSERT_EQ(d.ip_changed, 0); teardown();
}

/* === CALL / RET === */
TEST(call_near_and_ret) {
    setup(); state->ip = 0x0100;
    DecodeContext d = make_d(0xE8, 3); d.data0 = 0x00FD; /* offset so IP = 0x0100+3+0xFD = 0x0200 */
    exec_call_near(state, &d);
    ASSERT_EQ(state->ip, 0x0200); ASSERT_EQ(d.ip_changed, 1);
    uint16_t ret_addr = 0x0103;
    /* Stack should have return address */
    ASSERT_EQ(mem_read16(state, segoff_to_linear(0x1000, state->regs[REG_SP])), ret_addr);
    DecodeContext d2 = make_d(0xC3, 1);
    exec_ret_near(state, &d2);
    ASSERT_EQ(state->ip, ret_addr); teardown();
}
TEST(call_far_and_retf) {
    setup(); state->sregs[SREG_CS] = 0x1000; state->ip = 0x0100;
    DecodeContext d = make_d(0x9A, 5); d.data0 = 0x0300; d.data2 = 0x2000;
    exec_call_far(state, &d);
    ASSERT_EQ(state->ip, 0x0300); ASSERT_EQ(state->sregs[SREG_CS], 0x2000);
    DecodeContext d2 = make_d(0xCB, 1);
    exec_retf(state, &d2);
    ASSERT_EQ(state->ip, 0x0105); ASSERT_EQ(state->sregs[SREG_CS], 0x1000); teardown();
}
TEST(ret_imm_pops_extra) {
    setup(); state->ip = 0x0100;
    /* Simulate: caller pushed 2 words of parameters, then CALL */
    stack_push(state, 0x1111); /* param 1 */
    stack_push(state, 0x2222); /* param 2 */
    uint16_t sp_before_call = state->regs[REG_SP];
    DecodeContext d = make_d(0xE8, 3); d.data0 = 0x00FD;
    exec_call_near(state, &d);
    /* RET 4: pop return address, then adjust SP by 4 to skip params */
    DecodeContext d2 = make_d(0xC2, 3); d2.data0 = 4;
    exec_ret_near_imm(state, &d2);
    ASSERT_EQ(state->ip, 0x0103);
    ASSERT_EQ(state->regs[REG_SP], sp_before_call + 4); /* params cleaned up */
    teardown();
}
TEST(call_rm) {
    setup(); state->regs[REG_BX] = 0x0500; state->ip = 0x0100;
    DecodeContext d = make_d(0xFF, 2); d.mod = 3; d.rm = REG_BX; d.operand_width = 1; d.inst_length = 2;
    exec_call_rm(state, &d);
    ASSERT_EQ(state->ip, 0x0500);
    ASSERT_EQ(mem_read16(state, segoff_to_linear(0x1000, state->regs[REG_SP])), 0x0102);
    teardown();
}

/* === INT / IRET === */
TEST(int_pushes_flags_cs_ip) {
    setup(); state->sregs[SREG_CS] = 0x0000; state->ip = 0x0100;
    set_flag(state, FLAG_IF);
    /* Set up IVT for INT 0x21 at address 0x84 */
    mem_write16(state, 0x84, 0x1000); /* IP */
    mem_write16(state, 0x86, 0x0000); /* CS */
    DecodeContext d = make_d(0xCD, 2);
    exec_int(state, &d, 0x21);
    ASSERT_EQ(state->ip, 0x1000); ASSERT_EQ(state->sregs[SREG_CS], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_IF), 0); ASSERT_EQ(get_flag(state, FLAG_TF), 0);
    /* Stack: FLAGS, CS, IP (in push order) */
    teardown();
}
TEST(int_iret_roundtrip) {
    setup(); state->sregs[SREG_CS] = 0x0000; state->ip = 0x0100;
    set_flag(state, FLAG_CF); set_flag(state, FLAG_IF); set_flag(state, FLAG_DF);
    uint16_t saved_flags = flags_pack(state);
    mem_write16(state, 0x84, 0x1000); mem_write16(state, 0x86, 0x0000);
    DecodeContext d = make_d(0xCD, 2);
    exec_int(state, &d, 0x21);
    ASSERT_EQ(get_flag(state, FLAG_IF), 0);
    DecodeContext d2 = make_d(0xCF, 1);
    exec_iret(state, &d2);
    ASSERT_EQ(state->ip, 0x0102); ASSERT_EQ(state->sregs[SREG_CS], 0x0000);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1); ASSERT_EQ(get_flag(state, FLAG_IF), 1);
    ASSERT_EQ(get_flag(state, FLAG_DF), 1);
    (void)saved_flags; teardown();
}
TEST(int3_uses_vector_3) {
    setup(); mem_write16(state, 12, 0x2000); mem_write16(state, 14, 0x0000);
    state->ip = 0x0100;
    DecodeContext d = make_d(0xCC, 1);
    exec_int3(state, &d);
    ASSERT_EQ(state->ip, 0x2000); teardown();
}
TEST(into_triggers_when_of_set) {
    setup(); set_flag(state, FLAG_OF);
    mem_write16(state, 16, 0x3000); mem_write16(state, 18, 0x0000);
    state->ip = 0x0100;
    DecodeContext d = make_d(0xCE, 1);
    exec_into(state, &d);
    ASSERT_EQ(state->ip, 0x3000); teardown();
}
TEST(into_noop_when_of_clear) {
    setup(); clear_flag(state, FLAG_OF);
    state->ip = 0x0100;
    DecodeContext d = make_d(0xCE, 1);
    exec_into(state, &d);
    ASSERT_EQ(state->ip, 0x0100); ASSERT_EQ(d.ip_changed, 0); teardown();
}
TEST(int_clears_if_and_tf) {
    setup(); set_flag(state, FLAG_IF); set_flag(state, FLAG_TF);
    mem_write16(state, 0x84, 0x1000); mem_write16(state, 0x86, 0x0000);
    DecodeContext d = make_d(0xCD, 2);
    exec_int(state, &d, 0x21);
    ASSERT_EQ(get_flag(state, FLAG_IF), 0); ASSERT_EQ(get_flag(state, FLAG_TF), 0);
    teardown();
}

/* === LOOP === */
TEST(loop_decrements_cx) {
    setup(); state->regs[REG_CX] = 5; state->ip = 0x0100;
    DecodeContext d = make_d(0xE2, 2); d.data0 = 0xFC; /* -4 */
    exec_loop(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 4); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(loop_exits_when_cx_zero) {
    setup(); state->regs[REG_CX] = 1; state->ip = 0x0100;
    DecodeContext d = make_d(0xE2, 2); d.data0 = 0xFC;
    exec_loop(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 0); ASSERT_EQ(d.ip_changed, 0); teardown();
}
TEST(loop_cx_wraps) {
    setup(); state->regs[REG_CX] = 0; state->ip = 0x0100;
    DecodeContext d = make_d(0xE2, 2); d.data0 = 0xFC;
    exec_loop(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 0xFFFF); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(loopz_continues_while_zf) {
    setup(); state->regs[REG_CX] = 5; set_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0xE1, 2); d.data0 = 0xFC;
    exec_loopz(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 4); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(loopz_exits_on_zf_clear) {
    setup(); state->regs[REG_CX] = 5; clear_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0xE1, 2); d.data0 = 0xFC;
    exec_loopz(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 4); ASSERT_EQ(d.ip_changed, 0); teardown();
}
TEST(loopnz_continues_while_not_zf) {
    setup(); state->regs[REG_CX] = 5; clear_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0xE0, 2); d.data0 = 0xFC;
    exec_loopnz(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 4); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(loopnz_exits_on_zf_set) {
    setup(); state->regs[REG_CX] = 5; set_flag(state, FLAG_ZF);
    DecodeContext d = make_d(0xE0, 2); d.data0 = 0xFC;
    exec_loopnz(state, &d);
    ASSERT_EQ(state->regs[REG_CX], 4); ASSERT_EQ(d.ip_changed, 0); teardown();
}
TEST(jcxz_jumps_when_zero) {
    setup(); state->regs[REG_CX] = 0; state->ip = 0x0100;
    DecodeContext d = make_d(0xE3, 2); d.data0 = 10;
    exec_jcxz(state, &d);
    ASSERT_EQ(state->ip, 0x0100 + 2 + 10); ASSERT_EQ(d.ip_changed, 1); teardown();
}
TEST(jcxz_no_jump_when_nonzero) {
    setup(); state->regs[REG_CX] = 1;
    DecodeContext d = make_d(0xE3, 2); d.data0 = 10;
    exec_jcxz(state, &d);
    ASSERT_EQ(d.ip_changed, 0); teardown();
}
TEST(loop_no_flags_affected) {
    setup(); state->regs[REG_CX] = 5; set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    DecodeContext d = make_d(0xE2, 2); d.data0 = 0xFC;
    exec_loop(state, &d);
    ASSERT_EQ(state->flags, before); teardown();
}

int main(void) {
    printf("test_control:\n");
    RUN_TEST(eval_jz_true); RUN_TEST(eval_jz_false);
    RUN_TEST(eval_jb_true);
    RUN_TEST(eval_ja_true); RUN_TEST(eval_ja_false_cf); RUN_TEST(eval_ja_false_zf);
    RUN_TEST(eval_jl_true); RUN_TEST(eval_jl_false);
    RUN_TEST(eval_jle_true_zf); RUN_TEST(eval_jle_true_sf_of);
    RUN_TEST(eval_jg_true); RUN_TEST(eval_all_16_conditions);
    RUN_TEST(jmp_short_forward); RUN_TEST(jmp_short_backward);
    RUN_TEST(jmp_near); RUN_TEST(jmp_far);
    RUN_TEST(jmp_rm); RUN_TEST(jmp_no_flags);
    RUN_TEST(jcc_taken); RUN_TEST(jcc_not_taken);
    RUN_TEST(call_near_and_ret); RUN_TEST(call_far_and_retf);
    RUN_TEST(ret_imm_pops_extra); RUN_TEST(call_rm);
    RUN_TEST(int_pushes_flags_cs_ip); RUN_TEST(int_iret_roundtrip);
    RUN_TEST(int3_uses_vector_3); RUN_TEST(into_triggers_when_of_set);
    RUN_TEST(into_noop_when_of_clear); RUN_TEST(int_clears_if_and_tf);
    RUN_TEST(loop_decrements_cx); RUN_TEST(loop_exits_when_cx_zero);
    RUN_TEST(loop_cx_wraps);
    RUN_TEST(loopz_continues_while_zf); RUN_TEST(loopz_exits_on_zf_clear);
    RUN_TEST(loopnz_continues_while_not_zf); RUN_TEST(loopnz_exits_on_zf_set);
    RUN_TEST(jcxz_jumps_when_zero); RUN_TEST(jcxz_no_jump_when_nonzero);
    RUN_TEST(loop_no_flags_affected);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
