#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/snapshot.h"
#include "../../src/emulator/snapshot.c"
#include <stdlib.h>
#include <string.h>

/* Allocate state on the heap (too large for stack) */
static Emu86State *alloc_state(void)
{
    Emu86State *s = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!s) {
        fprintf(stderr, "Failed to allocate Emu86State\n");
        exit(1);
    }
    return s;
}

TEST(snapshot_round_trip)
{
    Emu86State *s1 = alloc_state();
    emu86_init(s1);

    /* Set various fields */
    s1->regs[REG_AX] = 0x1234;
    s1->regs[REG_BX] = 0x5678;
    s1->sregs[SREG_CS] = 0xF000;
    s1->ip = 0x0100;
    s1->flags = FLAG_IF | FLAG_ZF;
    s1->inst_count = 1000000;
    s1->io_ports[0x300] = 0x42;
    s1->trap_flag = 1;
    s1->seg_override_en = 2;
    s1->seg_override = SREG_ES;
    s1->rep_override_en = 1;
    s1->rep_mode = 1;
    s1->disk[0].size = 10485760;
    s1->disk[0].cylinders = 306;
    s1->disk[0].heads = 4;
    s1->disk[0].sectors = 17;
    s1->pit[0].reload = 0xFFFF;
    s1->pit[0].counter = 0x1234;
    s1->pit[0].mode = 3;
    s1->pit_lobyte_pending = 1;
    s1->kb_buffer[0] = 0x1E;
    s1->kb_head = 1;
    s1->video_mode = 3;
    s1->cursor_row = 10;
    s1->cursor_col = 40;
    s1->spkr_en = 3;
    s1->wave_counter = 500;
    s1->graphics_x = 720;
    s1->graphics_y = 348;
    s1->int8_asap = 1;

    /* Write a known pattern to memory */
    for (int i = 0; i < 256; i++)
        s1->mem[0x100 + i] = (uint8_t)i;

    /* Save */
    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    uint32_t written = emu86_snapshot_save(s1, buf, size);
    ASSERT(written == size);

    /* Restore into a fresh state */
    Emu86State *s2 = alloc_state();
    int rc = emu86_snapshot_restore(s2, buf, size);
    ASSERT_EQ(rc, 0);

    /* Verify CPU */
    ASSERT_EQ(s2->regs[REG_AX], 0x1234);
    ASSERT_EQ(s2->regs[REG_BX], 0x5678);
    ASSERT_EQ(s2->sregs[SREG_CS], 0xF000);
    ASSERT_EQ(s2->ip, 0x0100);
    ASSERT_EQ(s2->flags, FLAG_IF | FLAG_ZF);

    /* Verify interrupt/control */
    ASSERT_EQ(s2->trap_flag, 1);

    /* Verify prefix state */
    ASSERT_EQ(s2->seg_override_en, 2);
    ASSERT_EQ(s2->seg_override, SREG_ES);
    ASSERT_EQ(s2->rep_override_en, 1);
    ASSERT_EQ(s2->rep_mode, 1);

    /* Verify memory pattern */
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(s2->mem[0x100 + i], (uint8_t)i);

    /* Verify I/O ports */
    ASSERT_EQ(s2->io_ports[0x300], 0x42);

    /* Verify disk */
    ASSERT_EQ(s2->disk[0].size, 10485760U);
    ASSERT_EQ(s2->disk[0].cylinders, 306);
    ASSERT_EQ(s2->disk[0].heads, 4);
    ASSERT_EQ(s2->disk[0].sectors, 17);

    /* Verify PIT */
    ASSERT_EQ(s2->pit[0].reload, 0xFFFF);
    ASSERT_EQ(s2->pit[0].counter, 0x1234);
    ASSERT_EQ(s2->pit[0].mode, 3);
    ASSERT_EQ(s2->pit_lobyte_pending, 1);

    /* Verify keyboard */
    ASSERT_EQ(s2->kb_buffer[0], 0x1E);
    ASSERT_EQ(s2->kb_head, 1);

    /* Verify video */
    ASSERT_EQ(s2->video_mode, 3);
    ASSERT_EQ(s2->cursor_row, 10);
    ASSERT_EQ(s2->cursor_col, 40);

    /* Verify audio */
    ASSERT_EQ(s2->spkr_en, 3);
    ASSERT_EQ(s2->wave_counter, 500);

    /* Verify graphics */
    ASSERT_EQ(s2->graphics_x, 720);
    ASSERT_EQ(s2->graphics_y, 348);

    /* Verify timing */
    ASSERT_EQ(s2->inst_count, 1000000ULL);
    ASSERT_EQ(s2->int8_asap, 1);

    free(s1);
    free(s2);
    free(buf);
}

TEST(snapshot_magic_check)
{
    Emu86State *s = alloc_state();
    emu86_init(s);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    emu86_snapshot_save(s, buf, size);

    /* Corrupt magic */
    buf[0] = 0xFF;
    ASSERT_EQ(emu86_snapshot_restore(s, buf, size), -1);

    free(s);
    free(buf);
}

TEST(snapshot_version_check)
{
    Emu86State *s = alloc_state();
    emu86_init(s);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    emu86_snapshot_save(s, buf, size);

    /* Change version to 99 (little-endian) */
    buf[4] = 99;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    ASSERT_EQ(emu86_snapshot_restore(s, buf, size), -2);

    free(s);
    free(buf);
}

TEST(snapshot_checksum_check)
{
    Emu86State *s = alloc_state();
    emu86_init(s);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    emu86_snapshot_save(s, buf, size);

    /* Flip one bit in the middle of the data */
    buf[size / 2] ^= 0x01;
    ASSERT_EQ(emu86_snapshot_restore(s, buf, size), -3);

    free(s);
    free(buf);
}

TEST(snapshot_size_deterministic)
{
    Emu86State *s1 = alloc_state();
    Emu86State *s2 = alloc_state();
    emu86_init(s1);
    emu86_init(s2);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf1 = (uint8_t *)malloc(size);
    uint8_t *buf2 = (uint8_t *)malloc(size);

    uint32_t w1 = emu86_snapshot_save(s1, buf1, size);
    uint32_t w2 = emu86_snapshot_save(s2, buf2, size);

    ASSERT_EQ(w1, w2);
    ASSERT(memcmp(buf1, buf2, w1) == 0);

    free(s1);
    free(s2);
    free(buf1);
    free(buf2);
}

TEST(snapshot_byte_order)
{
    Emu86State *s = alloc_state();
    emu86_init(s);
    s->regs[REG_AX] = 0x1234;

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    emu86_snapshot_save(s, buf, size);

    /* AX is the first field after the 12-byte header */
    ASSERT_EQ(buf[12], 0x34);  /* low byte first (little-endian) */
    ASSERT_EQ(buf[13], 0x12);  /* high byte second */

    free(s);
    free(buf);
}

TEST(snapshot_size_query)
{
    uint32_t size = emu86_snapshot_size();
    ASSERT(size > 0);

    Emu86State *s = alloc_state();
    emu86_init(s);
    uint8_t *buf = (uint8_t *)malloc(size);
    uint32_t written = emu86_snapshot_save(s, buf, size);
    ASSERT_EQ(size, written);

    free(s);
    free(buf);
}

TEST(snapshot_buffer_too_small)
{
    Emu86State *s = alloc_state();
    emu86_init(s);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);

    /* Buffer too small */
    uint32_t written = emu86_snapshot_save(s, buf, size - 1);
    ASSERT_EQ(written, 0U);

    free(s);
    free(buf);
}

int main(void)
{
    printf("test_snapshot:\n");

    RUN_TEST(snapshot_round_trip);
    RUN_TEST(snapshot_magic_check);
    RUN_TEST(snapshot_version_check);
    RUN_TEST(snapshot_checksum_check);
    RUN_TEST(snapshot_size_deterministic);
    RUN_TEST(snapshot_byte_order);
    RUN_TEST(snapshot_size_query);
    RUN_TEST(snapshot_buffer_too_small);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
