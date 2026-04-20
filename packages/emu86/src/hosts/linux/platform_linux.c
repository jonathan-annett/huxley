#define _POSIX_C_SOURCE 200809L
#include "platform_linux.h"
#include "platform.h"

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Terminal
 * ================================================================ */

static struct termios orig_termios;
static int terminal_raw = 0;

void terminal_init(void)
{
    if (terminal_raw) return;
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    /* Match 8086tiny: cbreak raw -echo min 0 */
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    terminal_raw = 1;
}

void terminal_restore(void)
{
    if (!terminal_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    /* Clear non-blocking flag */
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) & ~O_NONBLOCK);
    terminal_raw = 0;
}

int terminal_read(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? c : -1;
}

/* ================================================================
 * Disk I/O
 * ================================================================ */

/* The disk images are stored as DiskImage structs passed via ctx */

int platform_disk_read(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    DiskImage *disks = (DiskImage *)ctx;
    if (drive < 0 || drive >= MAX_DISKS || disks[drive].fd <= 0)
        return -1;
    if (lseek(disks[drive].fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = read(disks[drive].fd, buf, len);
    return (n >= 0) ? 0 : -1;
}

int platform_disk_write(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx)
{
    DiskImage *disks = (DiskImage *)ctx;
    if (drive < 0 || drive >= MAX_DISKS || disks[drive].fd <= 0)
        return -1;
    if (lseek(disks[drive].fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = write(disks[drive].fd, buf, len);
    return (n >= 0) ? 0 : -1;
}

/* ================================================================
 * Timer
 * ================================================================ */

uint64_t platform_get_time_us(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ================================================================
 * Console service
 * ================================================================ */

void service_keyboard(Emu86Platform *p)
{
    int ch;
    while ((ch = terminal_read()) >= 0) {
        ringbuf_write(&p->console_in, (uint8_t)ch);
    }
}

void flush_console(Emu86Platform *p)
{
    uint8_t buf[256];
    uint32_t avail = ringbuf_available(&p->console_out);
    while (avail > 0) {
        uint32_t chunk = avail > sizeof(buf) ? sizeof(buf) : avail;
        /* Read from ring buffer and write to stdout */
        for (uint32_t i = 0; i < chunk; i++) {
            uint8_t byte;
            if (ringbuf_read(&p->console_out, &byte) == 0)
                buf[i] = byte;
            else
                break;
        }
        write(STDOUT_FILENO, buf, chunk);
        avail = ringbuf_available(&p->console_out);
    }
}

/* ================================================================
 * Ring buffer allocation
 * ================================================================ */

void alloc_ring_buffer(Emu86RingBuf *rb, uint32_t size)
{
    rb->buf = (uint8_t *)calloc(size, 1);
    rb->size = size;
    rb->head = (volatile uint32_t *)calloc(1, sizeof(uint32_t));
    rb->tail = (volatile uint32_t *)calloc(1, sizeof(uint32_t));
    *rb->head = 0;
    *rb->tail = 0;
}

void free_ring_buffer(Emu86RingBuf *rb)
{
    free(rb->buf);
    free((void *)rb->head);
    free((void *)rb->tail);
    rb->buf = NULL;
    rb->head = NULL;
    rb->tail = NULL;
}
