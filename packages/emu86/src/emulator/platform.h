#ifndef EMU86_PLATFORM_H
#define EMU86_PLATFORM_H

#include <stdint.h>

/*
 * Emu86RingBuf — lock-free single-producer single-consumer ring buffer.
 *
 * Used for all I/O: console in/out, NIC tx/rx. The buffer, head, and
 * tail pointers are provided by the host:
 *   - Linux: malloc'd memory with regular uint32_t counters
 *   - Browser: SharedArrayBuffer views with Atomics on head/tail
 *
 * The emulator code is identical either way.
 *
 * Size MUST be a power of 2 (wrapping uses & (size-1), not modulo).
 * One slot is always empty (sentinel) to distinguish full from empty.
 * Capacity = size - 1.
 */
typedef struct {
    uint8_t          *buf;
    uint32_t          size;   /* must be power of 2 */
    volatile uint32_t *head;  /* producer writes here */
    volatile uint32_t *tail;  /* consumer reads here */
} Emu86RingBuf;

/* --- Ring buffer inline helpers --- */

static inline uint32_t ringbuf_available(const Emu86RingBuf *rb)
{
    return (*rb->head - *rb->tail) & (rb->size - 1);
}

static inline uint32_t ringbuf_free(const Emu86RingBuf *rb)
{
    return rb->size - 1 - ringbuf_available(rb);
}

/* Write one byte. Returns 0 on success, -1 if full. */
static inline int ringbuf_write(Emu86RingBuf *rb, uint8_t byte)
{
    uint32_t h = *rb->head;
    uint32_t next = (h + 1) & (rb->size - 1);
    if (next == *rb->tail)
        return -1; /* full */
    rb->buf[h] = byte;
    *rb->head = next;
    return 0;
}

/* Read one byte. Returns 0 on success, -1 if empty. */
static inline int ringbuf_read(Emu86RingBuf *rb, uint8_t *byte)
{
    uint32_t t = *rb->tail;
    if (t == *rb->head)
        return -1; /* empty */
    *byte = rb->buf[t];
    *rb->tail = (t + 1) & (rb->size - 1);
    return 0;
}

/* Write a block of bytes. Returns 0 on success, -1 if not enough space. */
static inline int ringbuf_write_buf(Emu86RingBuf *rb, const uint8_t *data, uint32_t len)
{
    if (ringbuf_free(rb) < len)
        return -1;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t h = *rb->head;
        rb->buf[h] = data[i];
        *rb->head = (h + 1) & (rb->size - 1);
    }
    return 0;
}

/* Read a block of bytes. Returns 0 on success, -1 if not enough data. */
static inline int ringbuf_read_buf(Emu86RingBuf *rb, uint8_t *data, uint32_t len)
{
    if (ringbuf_available(rb) < len)
        return -1;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t t = *rb->tail;
        data[i] = rb->buf[t];
        *rb->tail = (t + 1) & (rb->size - 1);
    }
    return 0;
}

/*
 * Emu86Platform — host abstraction interface.
 *
 * The emulator never calls the OS directly. The host provides disk
 * callbacks, I/O ring buffers, and a time source. A snapshot taken
 * in the browser loads on Linux and vice versa because the emulator
 * only touches this struct, never raw syscalls.
 */
typedef struct {
    /* --- Disk I/O --- */
    int (*disk_read)(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx);
    int (*disk_write)(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx);

    /* --- Console I/O (ring buffers) --- */
    Emu86RingBuf console_out;  /* emulator → host (character output) */
    Emu86RingBuf console_in;   /* host → emulator (keyboard input) */

    /* --- Network adapter (virtual NIC), NULL if not attached --- */
    struct {
        Emu86RingBuf tx;       /* emulator → host (outbound frames) */
        Emu86RingBuf rx;       /* host → emulator (inbound frames) */
        uint8_t  mac[6];
        uint8_t  irq;
    } *nic;

    /* --- Timer --- */
    uint64_t (*get_time_us)(void *ctx);  /* monotonic microseconds */

    /* --- Host context (opaque, passed to all callbacks) --- */
    void *ctx;

} Emu86Platform;

#endif /* EMU86_PLATFORM_H */
