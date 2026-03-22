#include "snapshot.h"
#include <string.h>

/* --- CRC32 (standard polynomial 0xEDB88320) --- */

static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void crc32_init(void)
{
    if (crc32_table_init)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

static uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* --- Little-endian serialisation helpers --- */

static void write_u8(uint8_t **p, uint8_t v)
{
    *(*p)++ = v;
}

static void write_u16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v);
    *(*p)++ = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v);
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)(v >> 16);
    *(*p)++ = (uint8_t)(v >> 24);
}

static void write_u64(uint8_t **p, uint64_t v)
{
    write_u32(p, (uint32_t)v);
    write_u32(p, (uint32_t)(v >> 32));
}

static void write_bytes(uint8_t **p, const uint8_t *src, uint32_t len)
{
    memcpy(*p, src, len);
    *p += len;
}

static uint8_t read_u8(const uint8_t **p)
{
    return *(*p)++;
}

static uint16_t read_u16(const uint8_t **p)
{
    uint16_t v = (*p)[0] | ((uint16_t)(*p)[1] << 8);
    *p += 2;
    return v;
}

static uint32_t read_u32(const uint8_t **p)
{
    uint32_t v = (*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static uint64_t read_u64(const uint8_t **p)
{
    uint32_t lo = read_u32(p);
    uint32_t hi = read_u32(p);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static void read_bytes(const uint8_t **p, uint8_t *dst, uint32_t len)
{
    memcpy(dst, *p, len);
    *p += len;
}

/* --- Data size calculation --- */

/*
 * Compute the size of the serialised field data (everything between
 * header and checksum).
 */
static uint32_t data_size(void)
{
    uint32_t s = 0;

    /* CPU */
    s += 8 * 2;  /* regs[8] */
    s += 4 * 2;  /* sregs[4] */
    s += 2;      /* ip */
    s += 2;      /* flags */

    /* Interrupt / control */
    s += 4 * 1;  /* halted, trap_flag, int_pending, int_vector */

    /* Prefix state */
    s += 4 * 1;  /* seg_override_en, seg_override, rep_override_en, rep_mode */

    /* Memory */
    s += EMU86_MEM_SIZE;

    /* I/O ports */
    s += EMU86_IO_PORTS;

    /* Disk: 3 * (4 + 2 + 1 + 1 + 4) = 3 * 12 */
    s += EMU86_NUM_DISKS * 12;

    /* PIT: 3 * (2 + 2 + 1) = 3 * 5 */
    s += EMU86_NUM_PIT * 5;
    s += 1;  /* pit_lobyte_pending */

    /* Keyboard */
    s += EMU86_KB_BUF_SIZE;
    s += 2;  /* kb_head, kb_tail */

    /* Video */
    s += 1;  /* video_mode */
    s += 2;  /* cursor_row */
    s += 2;  /* cursor_col */

    /* Audio */
    s += 1;  /* spkr_en */
    s += 2;  /* wave_counter */

    /* Graphics */
    s += 2;  /* graphics_x */
    s += 2;  /* graphics_y */

    /* Timing */
    s += 8;  /* inst_count */
    s += 1;  /* int8_asap */

    return s;
}

#define HEADER_SIZE 12  /* magic(4) + version(4) + data_size(4) */
#define CRC_SIZE     4

uint32_t emu86_snapshot_size(void)
{
    return HEADER_SIZE + data_size() + CRC_SIZE;
}

uint32_t emu86_snapshot_save(const Emu86State *state, uint8_t *buf, uint32_t buf_size)
{
    uint32_t total = emu86_snapshot_size();
    if (buf_size < total)
        return 0;

    uint8_t *p = buf;
    uint32_t ds = data_size();

    /* Header */
    write_u32(&p, EMU86_SNAPSHOT_MAGIC);
    write_u32(&p, EMU86_SNAPSHOT_VERSION);
    write_u32(&p, ds);

    /* CPU */
    for (int i = 0; i < 8; i++)
        write_u16(&p, state->regs[i]);
    for (int i = 0; i < 4; i++)
        write_u16(&p, state->sregs[i]);
    write_u16(&p, state->ip);
    write_u16(&p, state->flags);

    /* Interrupt / control */
    write_u8(&p, state->halted);
    write_u8(&p, state->trap_flag);
    write_u8(&p, state->int_pending);
    write_u8(&p, state->int_vector);

    /* Prefix state */
    write_u8(&p, state->seg_override_en);
    write_u8(&p, state->seg_override);
    write_u8(&p, state->rep_override_en);
    write_u8(&p, state->rep_mode);

    /* Memory */
    write_bytes(&p, state->mem, EMU86_MEM_SIZE);

    /* I/O ports */
    write_bytes(&p, state->io_ports, EMU86_IO_PORTS);

    /* Disk */
    for (int i = 0; i < EMU86_NUM_DISKS; i++) {
        write_u32(&p, state->disk[i].size);
        write_u16(&p, state->disk[i].cylinders);
        write_u8(&p, state->disk[i].heads);
        write_u8(&p, state->disk[i].sectors);
        write_u32(&p, state->disk[i].position);
    }

    /* PIT */
    for (int i = 0; i < EMU86_NUM_PIT; i++) {
        write_u16(&p, state->pit[i].reload);
        write_u16(&p, state->pit[i].counter);
        write_u8(&p, state->pit[i].mode);
    }
    write_u8(&p, state->pit_lobyte_pending);

    /* Keyboard */
    write_bytes(&p, state->kb_buffer, EMU86_KB_BUF_SIZE);
    write_u8(&p, state->kb_head);
    write_u8(&p, state->kb_tail);

    /* Video */
    write_u8(&p, state->video_mode);
    write_u16(&p, state->cursor_row);
    write_u16(&p, state->cursor_col);

    /* Audio */
    write_u8(&p, state->spkr_en);
    write_u16(&p, state->wave_counter);

    /* Graphics */
    write_u16(&p, state->graphics_x);
    write_u16(&p, state->graphics_y);

    /* Timing */
    write_u64(&p, state->inst_count);
    write_u8(&p, state->int8_asap);

    /* CRC32 over everything before the checksum */
    uint32_t crc = crc32_compute(buf, (uint32_t)(p - buf));
    write_u32(&p, crc);

    return total;
}

int emu86_snapshot_restore(Emu86State *state, const uint8_t *buf, uint32_t buf_size)
{
    if (buf_size < HEADER_SIZE + CRC_SIZE)
        return -4;

    const uint8_t *p = buf;

    /* Read and validate header */
    uint32_t magic = read_u32(&p);
    if (magic != EMU86_SNAPSHOT_MAGIC)
        return -1;

    uint32_t version = read_u32(&p);
    if (version != EMU86_SNAPSHOT_VERSION)
        return -2;

    uint32_t ds = read_u32(&p);
    uint32_t expected_total = HEADER_SIZE + ds + CRC_SIZE;
    if (buf_size < expected_total)
        return -4;

    /* Verify CRC32 */
    uint32_t stored_crc = (uint32_t)buf[expected_total - 4] |
                          ((uint32_t)buf[expected_total - 3] << 8) |
                          ((uint32_t)buf[expected_total - 2] << 16) |
                          ((uint32_t)buf[expected_total - 1] << 24);
    uint32_t computed_crc = crc32_compute(buf, expected_total - CRC_SIZE);
    if (stored_crc != computed_crc)
        return -3;

    /* CPU */
    for (int i = 0; i < 8; i++)
        state->regs[i] = read_u16(&p);
    for (int i = 0; i < 4; i++)
        state->sregs[i] = read_u16(&p);
    state->ip = read_u16(&p);
    state->flags = read_u16(&p);

    /* Interrupt / control */
    state->halted = read_u8(&p);
    state->trap_flag = read_u8(&p);
    state->int_pending = read_u8(&p);
    state->int_vector = read_u8(&p);

    /* Prefix state */
    state->seg_override_en = read_u8(&p);
    state->seg_override = read_u8(&p);
    state->rep_override_en = read_u8(&p);
    state->rep_mode = read_u8(&p);

    /* Memory */
    read_bytes(&p, state->mem, EMU86_MEM_SIZE);

    /* I/O ports */
    read_bytes(&p, state->io_ports, EMU86_IO_PORTS);

    /* Disk */
    for (int i = 0; i < EMU86_NUM_DISKS; i++) {
        state->disk[i].size = read_u32(&p);
        state->disk[i].cylinders = read_u16(&p);
        state->disk[i].heads = read_u8(&p);
        state->disk[i].sectors = read_u8(&p);
        state->disk[i].position = read_u32(&p);
    }

    /* PIT */
    for (int i = 0; i < EMU86_NUM_PIT; i++) {
        state->pit[i].reload = read_u16(&p);
        state->pit[i].counter = read_u16(&p);
        state->pit[i].mode = read_u8(&p);
    }
    state->pit_lobyte_pending = read_u8(&p);

    /* Keyboard */
    read_bytes(&p, state->kb_buffer, EMU86_KB_BUF_SIZE);
    state->kb_head = read_u8(&p);
    state->kb_tail = read_u8(&p);

    /* Video */
    state->video_mode = read_u8(&p);
    state->cursor_row = read_u16(&p);
    state->cursor_col = read_u16(&p);

    /* Audio */
    state->spkr_en = read_u8(&p);
    state->wave_counter = read_u16(&p);

    /* Graphics */
    state->graphics_x = read_u16(&p);
    state->graphics_y = read_u16(&p);

    /* Timing */
    state->inst_count = read_u64(&p);
    state->int8_asap = read_u8(&p);

    return 0;
}
