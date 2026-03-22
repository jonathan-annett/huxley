#ifndef EMU86_OPCODES_FLAGS_IO_H
#define EMU86_OPCODES_FLAGS_IO_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "../platform.h"
#include "helpers.h"

/* ================================================================
 * Flag manipulation — each affects only the named flag
 * ================================================================ */

static inline void exec_clc(Emu86State *s) { clear_flag(s, FLAG_CF); }
static inline void exec_stc(Emu86State *s) { set_flag(s, FLAG_CF); }
static inline void exec_cmc(Emu86State *s) { s->flags ^= FLAG_CF; }
static inline void exec_cli(Emu86State *s) { clear_flag(s, FLAG_IF); }
static inline void exec_sti(Emu86State *s) { set_flag(s, FLAG_IF); }
static inline void exec_cld(Emu86State *s) { clear_flag(s, FLAG_DF); }
static inline void exec_std(Emu86State *s) { set_flag(s, FLAG_DF); }

/* ================================================================
 * I/O port hook — stub, filled in by device emulation later
 * ================================================================ */

static inline void
io_port_write_hook(Emu86State *s, uint16_t port, uint8_t value)
{
    (void)s; (void)port; (void)value;
}

/* ================================================================
 * I/O port instructions — no flags affected
 * ================================================================ */

static inline void exec_in_al_imm(Emu86State *s, uint8_t port) {
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | s->io_ports[port];
}
static inline void exec_in_ax_imm(Emu86State *s, uint8_t port) {
    s->regs[REG_AX] = s->io_ports[port] | ((uint16_t)s->io_ports[port + 1] << 8);
}
static inline void exec_in_al_dx(Emu86State *s) {
    uint16_t port = s->regs[REG_DX];
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | s->io_ports[port];
}
static inline void exec_in_ax_dx(Emu86State *s) {
    uint16_t port = s->regs[REG_DX];
    s->regs[REG_AX] = s->io_ports[port] | ((uint16_t)s->io_ports[port + 1] << 8);
}

static inline void exec_out_imm_al(Emu86State *s, uint8_t port) {
    s->io_ports[port] = (uint8_t)s->regs[REG_AX];
    io_port_write_hook(s, port, s->io_ports[port]);
}
static inline void exec_out_imm_ax(Emu86State *s, uint8_t port) {
    s->io_ports[port] = (uint8_t)s->regs[REG_AX];
    s->io_ports[port + 1] = (uint8_t)(s->regs[REG_AX] >> 8);
    io_port_write_hook(s, port, s->io_ports[port]);
    io_port_write_hook(s, port + 1, s->io_ports[port + 1]);
}
static inline void exec_out_dx_al(Emu86State *s) {
    uint16_t port = s->regs[REG_DX];
    s->io_ports[port] = (uint8_t)s->regs[REG_AX];
    io_port_write_hook(s, port, s->io_ports[port]);
}
static inline void exec_out_dx_ax(Emu86State *s) {
    uint16_t port = s->regs[REG_DX];
    s->io_ports[port] = (uint8_t)s->regs[REG_AX];
    s->io_ports[port + 1] = (uint8_t)(s->regs[REG_AX] >> 8);
    io_port_write_hook(s, port, s->io_ports[port]);
    io_port_write_hook(s, port + 1, s->io_ports[port + 1]);
}

/* ================================================================
 * Miscellaneous
 * ================================================================ */

/* HLT: set halted flag (run loop yields) */
static inline void exec_hlt(Emu86State *s) {
    s->halted = 1;
}

/* SALC: undocumented — AL = CF ? 0xFF : 0x00, no flags */
static inline void exec_salc(Emu86State *s) {
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) |
                       (get_flag(s, FLAG_CF) ? 0xFF : 0x00);
}

/* ================================================================
 * Prefix handlers
 * ================================================================ */

static inline void exec_segment_override(Emu86State *s, uint8_t sreg) {
    s->seg_override_en = 2;
    s->seg_override = sreg;
}

static inline void exec_rep_prefix(Emu86State *s, uint8_t mode) {
    s->rep_override_en = 2;
    s->rep_mode = mode;
}

/* ================================================================
 * Emulator BIOS calls (0x0F xx)
 * ================================================================ */

/* PUTCHAR_AL: write AL to console_out ring buffer */
static inline void
exec_bios_putchar(Emu86State *s, Emu86Platform *p)
{
    uint8_t ch = (uint8_t)s->regs[REG_AX];
    ringbuf_write(&p->console_out, ch);
}

/* GET_RTC: write time to ES:BX. Simplified implementation. */
static inline void
exec_bios_get_rtc(Emu86State *s, Emu86Platform *p)
{
    uint64_t us = 0;
    if (p->get_time_us)
        us = p->get_time_us(p->ctx);

    /* Convert microseconds to seconds and write a simplified struct tm.
     * The BIOS expects struct tm layout at ES:BX. We write the fields
     * the BIOS actually uses. struct tm is 36+ bytes on most platforms.
     * We write zeros for most fields and populate seconds/minutes/hours. */
    uint32_t addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_BX]);
    uint32_t total_secs = (uint32_t)(us / 1000000ULL);
    uint32_t secs = total_secs % 60;
    uint32_t mins = (total_secs / 60) % 60;
    uint32_t hours = (total_secs / 3600) % 24;

    /* struct tm layout (int fields, 4 bytes each on most platforms):
     * [0]  tm_sec, [4]  tm_min, [8]  tm_hour, [12] tm_mday,
     * [16] tm_mon, [20] tm_year, [24] tm_wday, [28] tm_yday,
     * [32] tm_isdst */
    for (int i = 0; i < 36; i++)
        mem_write8(s, addr + i, 0);

    /* Write as 32-bit LE ints (matching struct tm int fields) */
    mem_write16(s, addr + 0, (uint16_t)secs);   mem_write16(s, addr + 2, 0);
    mem_write16(s, addr + 4, (uint16_t)mins);    mem_write16(s, addr + 6, 0);
    mem_write16(s, addr + 8, (uint16_t)hours);   mem_write16(s, addr + 10, 0);

    /* Milliseconds at offset 36 (2 bytes, as in original) */
    uint16_t ms = (uint16_t)((us / 1000ULL) % 1000ULL);
    mem_write16(s, addr + 36, ms);
}

/* DISK_READ: read from disk into ES:BX. Returns 0 on success. */
static inline int
exec_bios_disk_read(Emu86State *s, Emu86Platform *p)
{
    if (!p->disk_read) return -1;
    uint8_t drive = (uint8_t)s->regs[REG_DX]; /* DL = drive */
    uint32_t offset = (uint32_t)s->regs[REG_BP] << 9; /* sector * 512 */
    uint16_t count = s->regs[REG_AX]; /* byte count */
    uint32_t addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_BX]);
    int rc = p->disk_read(drive, offset, s->mem + addr, count, p->ctx);
    /* AL = bytes read on success, 0 on error (matching original ~) */
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | (rc >= 0 ? (uint8_t)count : 0);
    return rc;
}

/* DISK_WRITE: write from ES:BX to disk. Returns 0 on success. */
static inline int
exec_bios_disk_write(Emu86State *s, Emu86Platform *p)
{
    if (!p->disk_write) return -1;
    uint8_t drive = (uint8_t)s->regs[REG_DX];
    uint32_t offset = (uint32_t)s->regs[REG_BP] << 9;
    uint16_t count = s->regs[REG_AX];
    uint32_t addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_BX]);
    int rc = p->disk_write(drive, offset, s->mem + addr, count, p->ctx);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | (rc >= 0 ? (uint8_t)count : 0);
    return rc;
}

#endif /* EMU86_OPCODES_FLAGS_IO_H */
