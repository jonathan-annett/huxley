#define _POSIX_C_SOURCE 200809L
#include "harness.h"
#include "../../src/emulator/state.h"
#include "../../src/emulator/platform.h"
#include "../../src/emulator/snapshot.h"
#include "../../src/emulator/snapshot.c"
#include "../../src/hosts/linux/platform_linux.h"
#include "../../src/hosts/linux/platform_linux.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST(ring_buffer_alloc_free)
{
    Emu86RingBuf rb;
    alloc_ring_buffer(&rb, 1024);
    ASSERT(rb.buf != NULL);
    ASSERT_EQ(rb.size, 1024U);
    ASSERT_EQ(*rb.head, 0U);
    ASSERT_EQ(*rb.tail, 0U);
    /* Write/read to verify it works */
    ASSERT_EQ(ringbuf_write(&rb, 0x42), 0);
    uint8_t val = 0;
    ASSERT_EQ(ringbuf_read(&rb, &val), 0);
    ASSERT_EQ(val, 0x42);
    free_ring_buffer(&rb);
}

TEST(disk_read_callback)
{
    /* Create temp file with known content */
    char tmppath[] = "/tmp/emu86_test_disk_XXXXXX";
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0);
    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)i;
    write(fd, data, 512);

    DiskImage disks[3] = {{0},{0},{0}};
    disks[0].fd = fd;
    disks[0].size = 512;

    uint8_t buf[512];
    memset(buf, 0, 512);
    int rc = platform_disk_read(0, 0, buf, 512, disks);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 512; i++)
        ASSERT_EQ(buf[i], (uint8_t)i);

    close(fd);
    unlink(tmppath);
}

TEST(disk_write_callback)
{
    char tmppath[] = "/tmp/emu86_test_disk_XXXXXX";
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0);
    /* Write 512 zeros as placeholder */
    uint8_t zeros[512]; memset(zeros, 0, 512);
    write(fd, zeros, 512);

    DiskImage disks[3] = {{0},{0},{0}};
    disks[0].fd = fd;
    disks[0].size = 512;

    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(0xA0 + (i & 0xFF));
    int rc = platform_disk_write(0, 0, data, 512, disks);
    ASSERT_EQ(rc, 0);

    /* Read back and verify */
    uint8_t verify[512];
    lseek(fd, 0, SEEK_SET);
    read(fd, verify, 512);
    for (int i = 0; i < 512; i++)
        ASSERT_EQ(verify[i], data[i]);

    close(fd);
    unlink(tmppath);
}

TEST(disk_read_at_offset)
{
    char tmppath[] = "/tmp/emu86_test_disk_XXXXXX";
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0);
    /* Write 2048 bytes with known pattern */
    uint8_t data[2048];
    for (int i = 0; i < 2048; i++) data[i] = (uint8_t)(i & 0xFF);
    write(fd, data, 2048);

    DiskImage disks[3] = {{0},{0},{0}};
    disks[0].fd = fd;
    disks[0].size = 2048;

    uint8_t buf[512];
    int rc = platform_disk_read(0, 1024, buf, 512, disks);
    ASSERT_EQ(rc, 0);
    for (int i = 0; i < 512; i++)
        ASSERT_EQ(buf[i], (uint8_t)((1024 + i) & 0xFF));

    close(fd);
    unlink(tmppath);
}

TEST(timer_returns_microseconds)
{
    uint64_t t1 = platform_get_time_us(NULL);
    usleep(10000); /* 10ms */
    uint64_t t2 = platform_get_time_us(NULL);
    ASSERT(t2 > t1);
    uint64_t diff = t2 - t1;
    ASSERT(diff >= 5000);   /* at least 5ms */
    ASSERT(diff < 100000);  /* less than 100ms */
}

TEST(snapshot_save_load_file)
{
    Emu86State *s1 = (Emu86State *)calloc(1, sizeof(Emu86State));
    emu86_init(s1);
    s1->regs[REG_AX] = 0xDEAD;
    s1->regs[REG_BX] = 0xBEEF;
    s1->sregs[SREG_CS] = 0xF000;
    s1->ip = 0x0100;

    /* Save to temp file */
    char tmppath[] = "/tmp/emu86_test_snap_XXXXXX";
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0);

    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    uint32_t written = emu86_snapshot_save(s1, buf, size);
    ASSERT(written > 0);
    write(fd, buf, written);
    close(fd);

    /* Load into fresh state */
    Emu86State *s2 = (Emu86State *)calloc(1, sizeof(Emu86State));
    FILE *f = fopen(tmppath, "rb");
    ASSERT(f != NULL);
    fread(buf, 1, size, f);
    fclose(f);
    int rc = emu86_snapshot_restore(s2, buf, size);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(s2->regs[REG_AX], 0xDEAD);
    ASSERT_EQ(s2->regs[REG_BX], 0xBEEF);
    ASSERT_EQ(s2->sregs[SREG_CS], 0xF000);
    ASSERT_EQ(s2->ip, 0x0100);

    free(s1); free(s2); free(buf);
    unlink(tmppath);
}

int main(void)
{
    printf("test_platform_linux:\n");
    RUN_TEST(ring_buffer_alloc_free);
    RUN_TEST(disk_read_callback);
    RUN_TEST(disk_write_callback);
    RUN_TEST(disk_read_at_offset);
    RUN_TEST(timer_returns_microseconds);
    RUN_TEST(snapshot_save_load_file);
    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
