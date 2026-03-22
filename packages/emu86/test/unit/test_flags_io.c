#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/decode.h"
#include "../../src/emulator/opcodes/helpers.h"
#include "../../src/emulator/opcodes/transfer.h"
#include "../../src/emulator/opcodes/flags_io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static Emu86State *state;
static void setup(void) {
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(state);
}
static void teardown(void) { free(state); state = NULL; }

/* === Flag manipulation === */
TEST(test_clc) {
    setup(); set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t other = state->flags & ~FLAG_CF;
    exec_clc(state);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0);
    ASSERT_EQ(state->flags & ~FLAG_CF, other); teardown();
}
TEST(test_stc) {
    setup(); clear_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t other = state->flags & ~FLAG_CF;
    exec_stc(state);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1);
    ASSERT_EQ(state->flags & ~FLAG_CF, other); teardown();
}
TEST(cmc_set_to_clear) {
    setup(); set_flag(state, FLAG_CF); exec_cmc(state);
    ASSERT_EQ(get_flag(state, FLAG_CF), 0); teardown();
}
TEST(cmc_clear_to_set) {
    setup(); clear_flag(state, FLAG_CF); exec_cmc(state);
    ASSERT_EQ(get_flag(state, FLAG_CF), 1); teardown();
}
TEST(test_cli) {
    setup(); set_flag(state, FLAG_IF); exec_cli(state);
    ASSERT_EQ(get_flag(state, FLAG_IF), 0); teardown();
}
TEST(test_sti) {
    setup(); clear_flag(state, FLAG_IF); exec_sti(state);
    ASSERT_EQ(get_flag(state, FLAG_IF), 1); teardown();
}
TEST(test_cld) {
    setup(); set_flag(state, FLAG_DF); exec_cld(state);
    ASSERT_EQ(get_flag(state, FLAG_DF), 0); teardown();
}
TEST(test_std) {
    setup(); clear_flag(state, FLAG_DF); exec_std(state);
    ASSERT_EQ(get_flag(state, FLAG_DF), 1); teardown();
}
TEST(flag_ops_preserve_other_flags) {
    setup();
    set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF); set_flag(state, FLAG_SF);
    set_flag(state, FLAG_OF); set_flag(state, FLAG_IF);
    uint16_t before = state->flags;
    exec_clc(state);
    ASSERT_EQ(state->flags, before & ~FLAG_CF);
    state->flags = before; /* restore */
    exec_sti(state); /* IF already set, should be no change */
    ASSERT_EQ(state->flags, before);
    teardown();
}

/* === I/O ports === */
TEST(in_al_imm) {
    setup(); state->io_ports[0x60] = 0x42;
    exec_in_al_imm(state, 0x60);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x42); teardown();
}
TEST(in_ax_imm) {
    setup(); state->io_ports[0x60] = 0x34; state->io_ports[0x61] = 0x12;
    exec_in_ax_imm(state, 0x60);
    ASSERT_EQ(state->regs[REG_AX], 0x1234); teardown();
}
TEST(in_al_dx) {
    setup(); state->regs[REG_DX] = 0x3D4; state->io_ports[0x3D4] = 0xAB;
    exec_in_al_dx(state);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0xAB); teardown();
}
TEST(in_ax_dx) {
    setup(); state->regs[REG_DX] = 0x3D4;
    state->io_ports[0x3D4] = 0xCD; state->io_ports[0x3D5] = 0xAB;
    exec_in_ax_dx(state);
    ASSERT_EQ(state->regs[REG_AX], 0xABCD); teardown();
}
TEST(out_imm_al) {
    setup(); state->regs[REG_AX] = 0x42;
    exec_out_imm_al(state, 0x60);
    ASSERT_EQ(state->io_ports[0x60], 0x42); teardown();
}
TEST(out_imm_ax) {
    setup(); state->regs[REG_AX] = 0x1234;
    exec_out_imm_ax(state, 0x60);
    ASSERT_EQ(state->io_ports[0x60], 0x34);
    ASSERT_EQ(state->io_ports[0x61], 0x12); teardown();
}
TEST(out_dx_al) {
    setup(); state->regs[REG_DX] = 0x3D4; state->regs[REG_AX] = 0x42;
    exec_out_dx_al(state);
    ASSERT_EQ(state->io_ports[0x3D4], 0x42); teardown();
}
TEST(out_dx_ax) {
    setup(); state->regs[REG_DX] = 0x3D4; state->regs[REG_AX] = 0x1234;
    exec_out_dx_ax(state);
    ASSERT_EQ(state->io_ports[0x3D4], 0x34);
    ASSERT_EQ(state->io_ports[0x3D5], 0x12); teardown();
}
TEST(io_no_flags) {
    setup(); set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    state->io_ports[0x60] = 0x42;
    exec_in_al_imm(state, 0x60);
    ASSERT_EQ(state->flags, before);
    exec_out_imm_al(state, 0x61);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === HLT === */
TEST(hlt_sets_halted) {
    setup(); ASSERT_EQ(state->halted, 0);
    exec_hlt(state);
    ASSERT_EQ(state->halted, 1); teardown();
}
TEST(hlt_no_flags) {
    setup(); set_flag(state, FLAG_CF); uint16_t before = state->flags;
    exec_hlt(state);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === SALC === */
TEST(salc_cf_set) {
    setup(); set_flag(state, FLAG_CF);
    exec_salc(state);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0xFF); teardown();
}
TEST(salc_cf_clear) {
    setup(); clear_flag(state, FLAG_CF);
    exec_salc(state);
    ASSERT_EQ((uint8_t)state->regs[REG_AX], 0x00); teardown();
}
TEST(salc_no_flags) {
    setup(); set_flag(state, FLAG_CF); set_flag(state, FLAG_ZF);
    uint16_t before = state->flags;
    exec_salc(state);
    ASSERT_EQ(state->flags, before); teardown();
}

/* === Prefix handlers === */
TEST(segment_override_sets_state) {
    setup();
    exec_segment_override(state, SREG_ES);
    ASSERT_EQ(state->seg_override_en, 2);
    ASSERT_EQ(state->seg_override, SREG_ES); teardown();
}
TEST(rep_prefix_sets_state) {
    setup();
    exec_rep_prefix(state, 1);
    ASSERT_EQ(state->rep_override_en, 2);
    ASSERT_EQ(state->rep_mode, 1); teardown();
}

/* === BIOS calls === */

/* Helper: create a simple ring buffer for testing */
static uint8_t rb_buf[256];
static uint32_t rb_head_val, rb_tail_val;
static Emu86RingBuf make_test_rb(void) {
    Emu86RingBuf rb;
    memset(rb_buf, 0, sizeof(rb_buf));
    rb_head_val = 0; rb_tail_val = 0;
    rb.buf = rb_buf; rb.size = 256;
    rb.head = &rb_head_val; rb.tail = &rb_tail_val;
    return rb;
}

TEST(bios_putchar) {
    setup();
    Emu86Platform p; memset(&p, 0, sizeof(p));
    p.console_out = make_test_rb();
    state->regs[REG_AX] = 0x41; /* AL = 'A' */
    exec_bios_putchar(state, &p);
    uint8_t ch = 0;
    ASSERT_EQ(ringbuf_read(&p.console_out, &ch), 0);
    ASSERT_EQ(ch, 0x41); teardown();
}
TEST(bios_putchar_multiple) {
    setup();
    Emu86Platform p; memset(&p, 0, sizeof(p));
    p.console_out = make_test_rb();
    state->regs[REG_AX] = 'H'; exec_bios_putchar(state, &p);
    state->regs[REG_AX] = 'i'; exec_bios_putchar(state, &p);
    uint8_t ch = 0;
    ringbuf_read(&p.console_out, &ch); ASSERT_EQ(ch, 'H');
    ringbuf_read(&p.console_out, &ch); ASSERT_EQ(ch, 'i');
    teardown();
}

/* Mock disk callbacks */
static uint8_t mock_disk_data[512];
static int mock_disk_read(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx) {
    (void)drive; (void)offset; (void)ctx;
    memcpy(buf, mock_disk_data, len < 512 ? len : 512);
    return 0;
}
static uint8_t mock_disk_written[512];
static int mock_disk_written_len = 0;
static int mock_disk_write(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx) {
    (void)drive; (void)offset; (void)ctx;
    memcpy(mock_disk_written, buf, len < 512 ? len : 512);
    mock_disk_written_len = len;
    return 0;
}

TEST(bios_disk_read) {
    setup();
    for (int i = 0; i < 512; i++) mock_disk_data[i] = (uint8_t)i;
    Emu86Platform p; memset(&p, 0, sizeof(p));
    p.disk_read = mock_disk_read;
    state->regs[REG_BP] = 0; /* sector 0 */
    state->regs[REG_AX] = 16; /* 16 bytes */
    state->sregs[SREG_ES] = 0; state->regs[REG_BX] = 0x500;
    state->regs[REG_DX] = 0; /* drive 0 */
    int rc = exec_bios_disk_read(state, &p);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 16; i++) ASSERT_EQ(mem_read8(state, 0x500 + i), (uint8_t)i);
    teardown();
}
TEST(bios_disk_write) {
    setup();
    Emu86Platform p; memset(&p, 0, sizeof(p));
    p.disk_write = mock_disk_write;
    for (int i = 0; i < 16; i++) mem_write8(state, 0x500 + i, (uint8_t)(0xA0 + i));
    state->regs[REG_BP] = 1; state->regs[REG_AX] = 16;
    state->sregs[SREG_ES] = 0; state->regs[REG_BX] = 0x500;
    state->regs[REG_DX] = 0;
    int rc = exec_bios_disk_write(state, &p);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 16; i++) ASSERT_EQ(mock_disk_written[i], (uint8_t)(0xA0 + i));
    teardown();
}

static uint64_t mock_get_time(void *ctx) { (void)ctx; return 3723000000ULL; } /* 1h 2m 3s */
TEST(bios_get_rtc) {
    setup();
    Emu86Platform p; memset(&p, 0, sizeof(p));
    p.get_time_us = mock_get_time;
    state->sregs[SREG_ES] = 0; state->regs[REG_BX] = 0x800;
    exec_bios_get_rtc(state, &p);
    /* Check tm_sec=3, tm_min=2, tm_hour=1 (4-byte LE ints) */
    ASSERT_EQ(mem_read16(state, 0x800), 3);  /* seconds */
    ASSERT_EQ(mem_read16(state, 0x804), 2);  /* minutes */
    ASSERT_EQ(mem_read16(state, 0x808), 1);  /* hours */
    teardown();
}

int main(void) {
    printf("test_flags_io:\n");
    RUN_TEST(test_clc); RUN_TEST(test_stc);
    RUN_TEST(cmc_set_to_clear); RUN_TEST(cmc_clear_to_set);
    RUN_TEST(test_cli); RUN_TEST(test_sti);
    RUN_TEST(test_cld); RUN_TEST(test_std);
    RUN_TEST(flag_ops_preserve_other_flags);
    RUN_TEST(in_al_imm); RUN_TEST(in_ax_imm);
    RUN_TEST(in_al_dx); RUN_TEST(in_ax_dx);
    RUN_TEST(out_imm_al); RUN_TEST(out_imm_ax);
    RUN_TEST(out_dx_al); RUN_TEST(out_dx_ax); RUN_TEST(io_no_flags);
    RUN_TEST(hlt_sets_halted); RUN_TEST(hlt_no_flags);
    RUN_TEST(salc_cf_set); RUN_TEST(salc_cf_clear); RUN_TEST(salc_no_flags);
    RUN_TEST(segment_override_sets_state); RUN_TEST(rep_prefix_sets_state);
    RUN_TEST(bios_putchar); RUN_TEST(bios_putchar_multiple);
    RUN_TEST(bios_disk_read); RUN_TEST(bios_disk_write);
    RUN_TEST(bios_get_rtc);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
