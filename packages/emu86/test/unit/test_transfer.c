#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/transfer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);
    /* Set up a usable stack */
    state->sregs[SREG_SS] = 0x1000;
    state->regs[REG_SP] = 0xFFFE;
}
static void teardown(void) { free(state); state = NULL; }

/* === Stack helpers === */

TEST(stack_push_pop_roundtrip) {
    setup();
    state->regs[REG_SP] = 0xFFFE;
    state->sregs[SREG_SS] = 0x1000;
    stack_push(state, 0x1234);
    ASSERT_EQ(state->regs[REG_SP], 0xFFFC);
    uint32_t addr = segoff_to_linear(0x1000, 0xFFFC);
    ASSERT_EQ(mem_read8(state, addr), 0x34);
    ASSERT_EQ(mem_read8(state, addr + 1), 0x12);
    uint16_t val = stack_pop(state);
    ASSERT_EQ(val, 0x1234);
    ASSERT_EQ(state->regs[REG_SP], 0xFFFE);
    teardown();
}

TEST(stack_push_multiple) {
    setup();
    stack_push(state, 0xAAAA);
    stack_push(state, 0xBBBB);
    ASSERT_EQ(stack_pop(state), 0xBBBB); /* LIFO */
    ASSERT_EQ(stack_pop(state), 0xAAAA);
    teardown();
}

TEST(stack_sp_wraps) {
    setup();
    state->regs[REG_SP] = 0x0000;
    stack_push(state, 0x1234);
    ASSERT_EQ(state->regs[REG_SP], 0xFFFE); /* wrapped */
    teardown();
}

/* === MOV === */

TEST(mov_reg_imm_byte) {
    setup();
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.reg4bit = 0; d.operand_width = 0; d.data0 = 0x42;
    exec_mov_reg_imm(state, &d);
    ASSERT_EQ(read_reg8(state, 0), 0x42);
    teardown();
}

TEST(mov_reg_imm_word) {
    setup();
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.reg4bit = REG_AX; d.operand_width = 1; d.data0 = 0x1234;
    exec_mov_reg_imm(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    teardown();
}

TEST(mov_no_flags) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    set_flag(state, FLAG_SF); set_flag(state, FLAG_OF);
    uint16_t before = state->flags;
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = REG_AX; d.reg = REG_BX;
    d.operand_width = 1; d.direction = 0;
    state->regs[REG_BX] = 0x5678;
    exec_mov_rm_reg(state, &d);
    ASSERT_EQ(state->flags, before);
    teardown();
}

TEST(mov_reg_to_mem) {
    setup();
    state->regs[REG_AX] = 0x5678;
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm_addr = 0x500; d.reg = REG_AX;
    d.operand_width = 1; d.direction = 0;
    exec_mov_rm_reg(state, &d);
    ASSERT_EQ(mem_read16(state, 0x500), 0x5678);
    teardown();
}

TEST(mov_mem_to_reg) {
    setup();
    mem_write16(state, 0x500, 0xABCD);
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm_addr = 0x500; d.reg = REG_AX;
    d.operand_width = 1; d.direction = 1;
    exec_mov_rm_reg(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0xABCD);
    teardown();
}

TEST(mov_sreg) {
    setup();
    state->regs[REG_AX] = 0x2000;
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 3; d.rm = REG_AX; d.reg = SREG_DS; d.operand_width = 1;
    exec_mov_sreg_rm(state, &d);
    ASSERT_EQ(state->sregs[SREG_DS], 0x2000);

    state->sregs[SREG_DS] = 0x3000;
    d.reg = SREG_DS; d.rm = REG_AX;
    exec_mov_rm_sreg(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x3000);
    teardown();
}

/* === PUSH / POP === */

TEST(push_pop_reg) {
    setup();
    state->regs[REG_AX] = 0x1234;
    exec_push_reg(state, REG_AX);
    state->regs[REG_AX] = 0;
    exec_pop_reg(state, REG_AX);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    teardown();
}

TEST(push_pop_sreg) {
    setup();
    state->sregs[SREG_DS] = 0x2000;
    exec_push_sreg(state, SREG_DS);
    state->sregs[SREG_DS] = 0;
    exec_pop_sreg(state, SREG_DS);
    ASSERT_EQ(state->sregs[SREG_DS], 0x2000);
    teardown();
}

TEST(pushf_popf_roundtrip) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    clear_flag(state, FLAG_SF); set_flag(state, FLAG_OF);
    exec_pushf(state);
    /* Clear all flags */
    state->flags = 0x0002; /* bit 1 always set */
    exec_popf(state);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_SF), 0);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1);
    teardown();
}

TEST(pushf_reserved_bits) {
    setup();
    state->flags = 0;
    exec_pushf(state);
    uint16_t pushed = mem_read16(state,
        segoff_to_linear(state->sregs[SREG_SS], state->regs[REG_SP]));
    ASSERT(pushed & 0x0002); /* bit 1 always 1 */
    ASSERT((pushed & 0xF000) == 0xF000); /* bits 12-15 always 1 on 8086 */
    teardown();
}

/* === XCHG === */

TEST(xchg_reg_reg) {
    setup();
    state->regs[REG_AX] = 0x1111;
    state->regs[REG_BX] = 0x2222;
    exec_xchg_ax_reg(state, REG_BX);
    ASSERT_EQ(state->regs[REG_AX], 0x2222);
    ASSERT_EQ(state->regs[REG_BX], 0x1111);
    teardown();
}

TEST(xchg_reg_mem) {
    setup();
    state->regs[REG_AX] = 0xAAAA;
    mem_write16(state, 0x600, 0xBBBB);
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm_addr = 0x600; d.reg = REG_AX; d.operand_width = 1;
    exec_xchg(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0xBBBB);
    ASSERT_EQ(mem_read16(state, 0x600), 0xAAAA);
    teardown();
}

TEST(xchg_no_flags) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    state->regs[REG_AX] = 0x1111; state->regs[REG_BX] = 0x2222;
    exec_xchg_ax_reg(state, REG_BX);
    ASSERT_EQ(state->flags, before);
    teardown();
}

TEST(xchg_ax_ax_is_nop) {
    setup();
    state->regs[REG_AX] = 0x1234;
    exec_xchg_ax_reg(state, REG_AX);
    ASSERT_EQ(state->regs[REG_AX], 0x1234);
    teardown();
}

/* === LEA === */

TEST(lea_basic) {
    setup();
    /* Simulate LEA AX, [BX+SI+0x10] with BX=0x100, SI=0x200, DS=0x0000 */
    state->regs[REG_BX] = 0x0100;
    state->regs[REG_SI] = 0x0200;
    state->sregs[SREG_DS] = 0x0000;
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 1; d.rm = 0; d.reg = REG_AX; d.operand_width = 1;
    /* rm_addr = DS:BX+SI+0x10 = 0x0310 (DS=0 so linear addr = offset) */
    d.rm_addr = segoff_to_linear(0x0000, 0x0310);
    exec_lea(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0310);
    teardown();
}

TEST(lea_no_memory_access) {
    setup();
    state->sregs[SREG_DS] = 0x0000;
    mem_write16(state, 0x500, 0xFFFF); /* fill with garbage */
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm = 6; d.reg = REG_AX; d.operand_width = 1;
    d.rm_addr = segoff_to_linear(0x0000, 0x0500);
    exec_lea(state, &d);
    ASSERT_EQ(state->regs[REG_AX], 0x0500); /* address, not 0xFFFF */
    teardown();
}

/* === LDS / LES === */

TEST(lds_basic) {
    setup();
    /* Store far pointer: offset=0x1234, segment=0x5678 */
    mem_write16(state, 0x800, 0x1234);
    mem_write16(state, 0x802, 0x5678);
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm_addr = 0x800; d.reg = REG_BX; d.operand_width = 1;
    exec_lds(state, &d);
    ASSERT_EQ(state->regs[REG_BX], 0x1234);
    ASSERT_EQ(state->sregs[SREG_DS], 0x5678);
    teardown();
}

TEST(les_basic) {
    setup();
    mem_write16(state, 0x800, 0xAAAA);
    mem_write16(state, 0x802, 0xBBBB);
    DecodeContext d; memset(&d, 0, sizeof(d));
    d.mod = 0; d.rm_addr = 0x800; d.reg = REG_BX; d.operand_width = 1;
    exec_les(state, &d);
    ASSERT_EQ(state->regs[REG_BX], 0xAAAA);
    ASSERT_EQ(state->sregs[SREG_ES], 0xBBBB);
    teardown();
}

/* === CBW / CWD === */

TEST(cbw_positive) {
    setup();
    state->regs[REG_AX] = 0xFF42; /* AL=0x42 (positive), AH=0xFF */
    exec_cbw(state);
    ASSERT_EQ(state->regs[REG_AX], 0x0042);
    teardown();
}

TEST(cbw_negative) {
    setup();
    state->regs[REG_AX] = 0x0080; /* AL=0x80 (negative) */
    exec_cbw(state);
    ASSERT_EQ(state->regs[REG_AX], 0xFF80);
    teardown();
}

TEST(cwd_positive) {
    setup();
    state->regs[REG_AX] = 0x1234;
    exec_cwd(state);
    ASSERT_EQ(state->regs[REG_DX], 0x0000);
    teardown();
}

TEST(cwd_negative) {
    setup();
    state->regs[REG_AX] = 0x8000;
    exec_cwd(state);
    ASSERT_EQ(state->regs[REG_DX], 0xFFFF);
    teardown();
}

/* === XLAT === */

TEST(xlat_basic) {
    setup();
    state->regs[REG_BX] = 0x200;
    state->regs[REG_AX] = 0x05; /* AL = 5 */
    state->sregs[SREG_DS] = 0x0000;
    mem_write8(state, 0x205, 0x42);
    exec_xlat(state);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x42);
    teardown();
}

/* === LAHF / SAHF === */

TEST(lahf_basic) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_PF); set_flag(state, FLAG_ZF);
    clear_flag(state, FLAG_SF);
    exec_lahf(state);
    uint8_t ah = (uint8_t)(state->regs[REG_AX] >> 8);
    ASSERT(ah & 0x01); /* CF */
    ASSERT(ah & 0x04); /* PF */
    ASSERT(ah & 0x40); /* ZF */
    ASSERT(!(ah & 0x80)); /* SF clear */
    teardown();
}

TEST(sahf_basic) {
    setup();
    /* AH = 0xD5: SF=1, ZF=1, AF=0, PF=1, CF=1 */
    state->regs[REG_AX] = (state->regs[REG_AX] & 0x00FF) | (0xD5 << 8);
    exec_sahf(state);
    ASSERT_EQ(get_flag(state, FLAG_SF), 1);
    ASSERT_EQ(get_flag(state, FLAG_ZF), 1);
    ASSERT_EQ(get_flag(state, FLAG_PF), 1);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(get_flag(state, FLAG_AF), 1); /* bit 4 of 0xD5 = 1 */
    teardown();
}

TEST(sahf_does_not_affect_of) {
    setup();
    set_flag(state, FLAG_OF);
    state->regs[REG_AX] = 0x0000; /* AH = 0 — clear all low flags */
    exec_sahf(state);
    ASSERT_EQ(get_flag(state, FLAG_OF), 1); /* OF unchanged */
    teardown();
}

int main(void) {
    printf("test_transfer:\n");
    RUN_TEST(stack_push_pop_roundtrip); RUN_TEST(stack_push_multiple);
    RUN_TEST(stack_sp_wraps);
    RUN_TEST(mov_reg_imm_byte); RUN_TEST(mov_reg_imm_word);
    RUN_TEST(mov_no_flags); RUN_TEST(mov_reg_to_mem);
    RUN_TEST(mov_mem_to_reg); RUN_TEST(mov_sreg);
    RUN_TEST(push_pop_reg); RUN_TEST(push_pop_sreg);
    RUN_TEST(pushf_popf_roundtrip); RUN_TEST(pushf_reserved_bits);
    RUN_TEST(xchg_reg_reg); RUN_TEST(xchg_reg_mem);
    RUN_TEST(xchg_no_flags); RUN_TEST(xchg_ax_ax_is_nop);
    RUN_TEST(lea_basic); RUN_TEST(lea_no_memory_access);
    RUN_TEST(lds_basic); RUN_TEST(les_basic);
    RUN_TEST(cbw_positive); RUN_TEST(cbw_negative);
    RUN_TEST(cwd_positive); RUN_TEST(cwd_negative);
    RUN_TEST(xlat_basic);
    RUN_TEST(lahf_basic); RUN_TEST(sahf_basic);
    RUN_TEST(sahf_does_not_affect_of);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
