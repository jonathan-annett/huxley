#include "harness.h"
#include "../../src/emulator/platform.h"
#include <stdlib.h>
#include <string.h>

/* Helper to create a ring buffer with given size (must be power of 2) */
static Emu86RingBuf make_ringbuf(uint32_t size)
{
    Emu86RingBuf rb;
    rb.buf = (uint8_t *)calloc(size, 1);
    rb.size = size;
    rb.head = (volatile uint32_t *)calloc(1, sizeof(uint32_t));
    rb.tail = (volatile uint32_t *)calloc(1, sizeof(uint32_t));
    *rb.head = 0;
    *rb.tail = 0;
    return rb;
}

static void free_ringbuf(Emu86RingBuf *rb)
{
    free(rb->buf);
    free((void *)rb->head);
    free((void *)rb->tail);
}

TEST(ringbuf_write_read_single)
{
    Emu86RingBuf rb = make_ringbuf(16);
    uint8_t val = 0;

    ASSERT_EQ(ringbuf_write(&rb, 0x42), 0);
    ASSERT_EQ(ringbuf_read(&rb, &val), 0);
    ASSERT_EQ(val, 0x42);

    free_ringbuf(&rb);
}

TEST(ringbuf_write_read_multiple)
{
    Emu86RingBuf rb = make_ringbuf(16);
    uint8_t val = 0;

    for (int i = 0; i < 10; i++)
        ASSERT_EQ(ringbuf_write(&rb, (uint8_t)i), 0);

    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(ringbuf_read(&rb, &val), 0);
        ASSERT_EQ(val, (uint8_t)i);
    }

    free_ringbuf(&rb);
}

TEST(ringbuf_empty_read_fails)
{
    Emu86RingBuf rb = make_ringbuf(16);
    uint8_t val = 0;

    ASSERT_EQ(ringbuf_read(&rb, &val), -1);

    free_ringbuf(&rb);
}

TEST(ringbuf_full_write_fails)
{
    Emu86RingBuf rb = make_ringbuf(16);

    /* Fill completely (capacity = size - 1 = 15) */
    for (int i = 0; i < 15; i++)
        ASSERT_EQ(ringbuf_write(&rb, (uint8_t)i), 0);

    /* One more should fail */
    ASSERT_EQ(ringbuf_write(&rb, 0xFF), -1);

    free_ringbuf(&rb);
}

TEST(ringbuf_wraps_around)
{
    Emu86RingBuf rb = make_ringbuf(16);
    uint8_t val = 0;

    /* Write 10 bytes, read them, so head/tail advance past initial position */
    for (int i = 0; i < 10; i++)
        ringbuf_write(&rb, (uint8_t)i);
    for (int i = 0; i < 10; i++)
        ringbuf_read(&rb, &val);

    /* Now write another 12 bytes — these will wrap around the buffer end */
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(ringbuf_write(&rb, (uint8_t)(0xA0 + i)), 0);

    /* Read them back and verify order */
    for (int i = 0; i < 12; i++) {
        ASSERT_EQ(ringbuf_read(&rb, &val), 0);
        ASSERT_EQ(val, (uint8_t)(0xA0 + i));
    }

    free_ringbuf(&rb);
}

TEST(ringbuf_available_and_free)
{
    Emu86RingBuf rb = make_ringbuf(16);

    for (int i = 0; i < 5; i++)
        ringbuf_write(&rb, (uint8_t)i);

    ASSERT_EQ(ringbuf_available(&rb), 5U);
    ASSERT_EQ(ringbuf_free(&rb), 10U);  /* 15 - 5 = 10 */

    free_ringbuf(&rb);
}

TEST(ringbuf_bulk_write_read)
{
    Emu86RingBuf rb = make_ringbuf(256);
    uint8_t src[100], dst[100];

    for (int i = 0; i < 100; i++)
        src[i] = (uint8_t)(i * 3 + 7);

    ASSERT_EQ(ringbuf_write_buf(&rb, src, 100), 0);
    ASSERT_EQ(ringbuf_available(&rb), 100U);

    ASSERT_EQ(ringbuf_read_buf(&rb, dst, 100), 0);
    ASSERT(memcmp(src, dst, 100) == 0);

    free_ringbuf(&rb);
}

int main(void)
{
    printf("test_ringbuf:\n");

    RUN_TEST(ringbuf_write_read_single);
    RUN_TEST(ringbuf_write_read_multiple);
    RUN_TEST(ringbuf_empty_read_fails);
    RUN_TEST(ringbuf_full_write_fails);
    RUN_TEST(ringbuf_wraps_around);
    RUN_TEST(ringbuf_available_and_free);
    RUN_TEST(ringbuf_bulk_write_read);

    printf("\n%d passed, %d failed\n", test_passes, test_failures);
    return test_failures ? 1 : 0;
}
