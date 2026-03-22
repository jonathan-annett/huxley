#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/tables.h"
#include "../../src/emulator/decode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Test infrastructure
 * ================================================================ */

static Emu86State  *state;
static Emu86Tables  tables;

/* Load the real BIOS binary and initialise tables */
static void setup(void)
{
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!state) { fprintf(stderr, "Failed to allocate state\n"); exit(1); }
    emu86_init(state);

    /* Load BIOS into F000:0100 (linear 0xF0100), matching the original */
    FILE *f = fopen("reference/bios", "rb");
    if (!f) {
        f = fopen("../../reference/bios", "rb"); /* try from test/unit/ cwd */
    }
    if (!f) { fprintf(stderr, "Cannot open reference/bios\n"); exit(1); }
    size_t n = fread(state->mem + 0xF0100, 1, 0xFF00, f);
    fclose(f);
    if (n == 0) { fprintf(stderr, "BIOS read failed\n"); exit(1); }

    /* Load decode tables from BIOS */
    emu86_load_tables(&tables, state);

    /* Set CS:IP to 0x0000:0x7C00 for test instructions */
    state->sregs[SREG_CS] = 0x0000;
    state->ip = 0x7C00;
}

static void teardown(void)
{
    free(state);
    state = NULL;
}

/* Place instruction bytes at CS:IP */
static void place_bytes(const uint8_t *bytes, int len)
{
    uint32_t addr = segoff_to_linear(state->sregs[SREG_CS], state->ip);
    /* Ensure some padding bytes after the instruction (for data reads) */
    memset(state->mem + addr, 0, 16);
    memcpy(state->mem + addr, bytes, len);
}

/* ================================================================
 * Tests
 * ================================================================ */

TEST(decode_nop)
{
    setup();
    uint8_t code[] = { 0x90 }; /* NOP */
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.opcode, 0x90);
    ASSERT_EQ(d.xlat_id, tables.data[TABLE_XLAT_OPCODE][0x90]);
    ASSERT_EQ(d.has_modrm, 0);
    ASSERT_EQ(d.inst_length, 1);
    teardown();
}

TEST(decode_mov_ax_imm16)
{
    setup();
    uint8_t code[] = { 0xB8, 0x34, 0x12 }; /* MOV AX, 0x1234 */
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.opcode, 0xB8);
    /* Raw decode: i_w = bit 0 of opcode. B8 = 10111000, reg4bit = 000, i_w = 0.
     * The execution code overrides i_w = !!(raw & 8) = 1 for this opcode class.
     * The decoder only does the raw extraction. */
    ASSERT_EQ(d.operand_width, 0);
    ASSERT_EQ(d.reg4bit, 0);
    /* inst_length depends on i_w from raw decode (0) + table values.
     * For B8, TABLE_BASE_INST_SIZE gives 1, TABLE_I_W_SIZE gives 1,
     * so length = 1 + 1*(0+1) = 2. The execution code adds the extra byte
     * when it overrides i_w to 1. */
    ASSERT_EQ(d.inst_length, 2);
    /* data0 should contain the 16-bit LE read at offset+1: 0x1234 */
    ASSERT_EQ((uint16_t)d.data0, 0x1234);
    teardown();
}

TEST(decode_mov_al_imm8)
{
    setup();
    uint8_t code[] = { 0xB0, 0x42 }; /* MOV AL, 0x42 */
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.opcode, 0xB0);
    ASSERT_EQ(d.reg4bit, 0);
    /* MOV AL,imm8: inst_length = base + i_w_size*(i_w+1).
     * For 0xB0, i_w = 0 from raw decode. Length should be 2. */
    ASSERT_EQ(d.inst_length, 2);
    ASSERT_EQ((uint8_t)(d.data0 & 0xFF), 0x42);
    teardown();
}

TEST(decode_add_rm_reg_direct)
{
    setup();
    /* ADD [BX+SI], AX = 01 00
     * 0x01: ADD r/m16, r16. ModRM = 0x00 → mod=00, reg=000(AX), rm=000(BX+SI) */
    uint8_t code[] = { 0x01, 0x00 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 0);
    ASSERT_EQ(d.reg, 0); /* AX */
    ASSERT_EQ(d.rm, 0);  /* BX+SI */
    ASSERT_EQ(d.direction, 0); /* rm ← reg (opcode 01: bit 1 = 0) */
    ASSERT_EQ(d.operand_width, 1); /* word (opcode 01: bit 0 = 1) */
    /* rm_addr should be a memory address (DS:BX+SI with BX=0,SI=0,DS=0) */
    ASSERT(d.mod < 3); /* memory operand */
    ASSERT_EQ(d.inst_length, 2);
    teardown();
}

TEST(decode_add_rm_reg_with_disp8)
{
    setup();
    /* ADD [BX+SI+0x10], AX: 01 40 10
     * 0x01: ADD r/m16, r16. ModRM = 0x40 → mod=01, reg=000(AX), rm=000(BX+SI), disp8=0x10 */
    uint8_t code[] = { 0x01, 0x40, 0x10 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 1);
    ASSERT_EQ(d.reg, 0);
    ASSERT_EQ(d.rm, 0);
    ASSERT_EQ(d.inst_length, 3);
    teardown();
}

TEST(decode_add_rm_reg_with_disp16)
{
    setup();
    /* ADD [BX+SI+0x1234], AX: 01 80 34 12
     * 0x01: ADD r/m16, r16. ModRM = 0x80 → mod=10, reg=000(AX), rm=000(BX+SI), disp16=0x1234 */
    uint8_t code[] = { 0x01, 0x80, 0x34, 0x12 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 2);
    ASSERT_EQ(d.reg, 0);
    ASSERT_EQ(d.rm, 0);
    ASSERT_EQ(d.inst_length, 4);
    teardown();
}

TEST(decode_modrm_register_mode)
{
    setup();
    /* ADD AX, BX: 01 D8
     * ModRM = 0xD8 → mod=11, reg=011(BX), rm=000(AX) */
    uint8_t code[] = { 0x01, 0xD8 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 3);
    ASSERT_EQ(d.reg, 3); /* BX */
    ASSERT_EQ(d.rm, 0);  /* AX */
    /* mod==3: rm_addr is the register index, not a memory address */
    ASSERT_EQ(d.rm_addr, 0U); /* AX register index */
    ASSERT_EQ(d.inst_length, 2);
    teardown();
}

TEST(decode_modrm_direct_address)
{
    setup();
    /* MOV AX, [0x1234]: A1 34 12
     * But wait, A1 is MOV AX,moffs16 which doesn't use ModRM.
     * Use instead: 8B 06 34 12 = MOV AX, [0x1234]
     * 0x8B: MOV r16, r/m16. ModRM = 0x06 → mod=00, reg=000(AX), rm=110(direct addr)
     * Followed by disp16 = 0x1234 */
    uint8_t code[] = { 0x8B, 0x06, 0x34, 0x12 };
    place_bytes(code, sizeof(code));

    /* DS = 0x0000 for this test */
    state->sregs[SREG_DS] = 0x0000;

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 0);
    ASSERT_EQ(d.rm, 6);  /* direct address special case */
    /* rm_addr should be DS:0x1234 = 0*16 + 0x1234 = 0x1234 */
    ASSERT_EQ(d.rm_addr, 0x1234U);
    /* inst_length: base + 2 (direct address displacement) */
    ASSERT_EQ(d.inst_length, 4);
    teardown();
}

TEST(decode_segment_override)
{
    setup();
    /* Set segment override to ES */
    state->seg_override_en = 2;
    state->seg_override = SREG_ES;
    state->sregs[SREG_ES] = 0x2000;
    state->sregs[SREG_DS] = 0x0000;

    /* 8B 06 34 12 = MOV AX, [0x1234] — normally uses DS, but ES override active */
    uint8_t code[] = { 0x8B, 0x06, 0x34, 0x12 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    /* With ES=0x2000, rm_addr should be ES:0x1234 = 0x2000*16 + 0x1234 = 0x21234 */
    ASSERT_EQ(d.rm_addr, segoff_to_linear(0x2000, 0x1234));
    teardown();
}

TEST(decode_reg8_mapping)
{
    setup();
    state->regs[REG_AX] = 0xAABB;
    state->regs[REG_CX] = 0xCCDD;

    /* Index 0 = AL (low byte of AX) */
    ASSERT_EQ(read_reg8(state, 0), 0xBB);
    /* Index 1 = CL (low byte of CX) */
    ASSERT_EQ(read_reg8(state, 1), 0xDD);
    /* Index 4 = AH (high byte of AX) */
    ASSERT_EQ(read_reg8(state, 4), 0xAA);
    /* Index 5 = CH (high byte of CX) */
    ASSERT_EQ(read_reg8(state, 5), 0xCC);
    teardown();
}

TEST(write_reg8_mapping)
{
    setup();
    state->regs[REG_AX] = 0x0000;

    write_reg8(state, 0, 0x42); /* AL */
    ASSERT_EQ(state->regs[REG_AX], 0x0042);

    write_reg8(state, 4, 0x99); /* AH */
    ASSERT_EQ(state->regs[REG_AX], 0x9942);
    teardown();
}

TEST(test_segoff_to_linear)
{
    ASSERT_EQ(segoff_to_linear(0xF000, 0x0100), 0xF0100U);
    ASSERT_EQ(segoff_to_linear(0x0000, 0x0000), 0x00000U);
    ASSERT_EQ(segoff_to_linear(0xFFFF, 0x000F), 0xFFFF0U + 0xFU);
    ASSERT_EQ(segoff_to_linear(0x1000, 0x0001), 0x10001U);
}

TEST(mem_read_write_8)
{
    setup();
    mem_write8(state, 0x100, 0x42);
    ASSERT_EQ(mem_read8(state, 0x100), 0x42);
    teardown();
}

TEST(mem_read_write_16)
{
    setup();
    mem_write16(state, 0x100, 0x1234);
    ASSERT_EQ(mem_read16(state, 0x100), 0x1234);
    /* Verify byte order (little-endian) */
    ASSERT_EQ(mem_read8(state, 0x100), 0x34); /* low byte */
    ASSERT_EQ(mem_read8(state, 0x101), 0x12); /* high byte */
    teardown();
}

TEST(inst_length_prefix_instructions)
{
    setup();
    /* ES: segment override prefix = 0x26 */
    uint8_t code[] = { 0x26, 0x00, 0x00, 0x00, 0x00, 0x00 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.opcode, 0x26);
    ASSERT_EQ(d.inst_length, 1);
    teardown();
}

TEST(inst_length_group_opcodes)
{
    setup();
    /* 0x80 = ALU r/m8, imm8. ModRM = 0xC0 → mod=11, reg=000(ADD), rm=000(AL)
     * Followed by imm8 = 0x05
     * Total: opcode(1) + modrm(1) + imm8(1) = 3 bytes */
    uint8_t code[] = { 0x80, 0xC0, 0x05, 0x00, 0x00, 0x00 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.opcode, 0x80);
    ASSERT_EQ(d.has_modrm, 1);
    ASSERT_EQ(d.mod, 3);
    ASSERT_EQ(d.inst_length, 3);

    /* Now try with memory operand + disp8:
     * 0x80 0x40 0x10 0x05 = ADD BYTE [BX+SI+0x10], 0x05
     * mod=01, reg=000, rm=000, disp8=0x10, imm8=0x05
     * Total: opcode(1) + modrm(1) + disp8(1) + imm8(1) = 4 */
    uint8_t code2[] = { 0x80, 0x40, 0x10, 0x05, 0x00, 0x00 };
    place_bytes(code2, sizeof(code2));

    decode_instruction(state, &tables, &d);
    ASSERT_EQ(d.inst_length, 4);

    teardown();
}

TEST(decode_effective_address_bx_si)
{
    setup();
    /* ADD [BX+SI], AL: 00 00
     * With BX=0x100, SI=0x200, DS=0x1000 → EA = DS:BX+SI = 0x10000 + 0x0300 = 0x10300 */
    state->regs[REG_BX] = 0x0100;
    state->regs[REG_SI] = 0x0200;
    state->sregs[SREG_DS] = 0x1000;

    uint8_t code[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.mod, 0);
    ASSERT_EQ(d.rm, 0); /* BX+SI */
    ASSERT_EQ(d.rm_addr, segoff_to_linear(0x1000, 0x0300));
    teardown();
}

TEST(decode_effective_address_bp_disp)
{
    setup();
    /* ADD [BP+0x20], AL: 00 46 20
     * mod=01, reg=000, rm=110(BP), disp8=0x20
     * With BP=0x0500, SS=0x2000 → EA = SS:BP+0x20 = 0x20000 + 0x0520 = 0x20520 */
    state->regs[REG_BP] = 0x0500;
    state->sregs[SREG_SS] = 0x2000;

    uint8_t code[] = { 0x00, 0x46, 0x20, 0x00, 0x00, 0x00 };
    place_bytes(code, sizeof(code));

    DecodeContext d;
    decode_instruction(state, &tables, &d);

    ASSERT_EQ(d.mod, 1);
    ASSERT_EQ(d.rm, 6); /* BP */
    ASSERT_EQ(d.rm_addr, segoff_to_linear(0x2000, 0x0520));
    teardown();
}

int main(void)
{
    printf("test_decode:\n");

    RUN_TEST(decode_nop);
    RUN_TEST(decode_mov_ax_imm16);
    RUN_TEST(decode_mov_al_imm8);
    RUN_TEST(decode_add_rm_reg_direct);
    RUN_TEST(decode_add_rm_reg_with_disp8);
    RUN_TEST(decode_add_rm_reg_with_disp16);
    RUN_TEST(decode_modrm_register_mode);
    RUN_TEST(decode_modrm_direct_address);
    RUN_TEST(decode_segment_override);
    RUN_TEST(decode_reg8_mapping);
    RUN_TEST(write_reg8_mapping);
    RUN_TEST(test_segoff_to_linear);
    RUN_TEST(mem_read_write_8);
    RUN_TEST(mem_read_write_16);
    RUN_TEST(inst_length_prefix_instructions);
    RUN_TEST(inst_length_group_opcodes);
    RUN_TEST(decode_effective_address_bx_si);
    RUN_TEST(decode_effective_address_bp_disp);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
