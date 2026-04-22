/*
 * overrides.c — deterministic replacements for non-deterministic syscalls
 * used by the reference emulator (8086tiny.c).
 *
 * The reference is compiled with -Dread=harness_read, -Dtime=harness_time,
 * etc. (see Makefile harness target). THIS file is compiled WITHOUT those
 * defines, so the forwarding wrappers can still call the real libc
 * functions for legitimate disk I/O.
 *
 * Non-determinism neutralised:
 *   - read(0, ...)    : keyboard stdin. Returns 0 (no key ever available).
 *   - time(...)       : returns 0 always.
 *   - ftime(...)      : zeroes the struct.
 *   - localtime(...)  : returns zero-filled struct tm.
 *
 * All disk reads (read(disk[n], ...)) pass through to the real syscall.
 */

#include <stdint.h>
#include <time.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <unistd.h>

/* EMU-32: defined in harness.c. Pulls one byte from the harness's keyboard
 * queue (filled by drain_kbd_fifo) and records it so the harness can push
 * the same byte into our emulator's console_in ringbuf before our matching
 * step runs. Returns 1 and writes *byte_out on success, 0 if queue empty. */
extern int harness_consume_kbd_byte(uint8_t *byte_out);

/* read(): forward to real libc read() for any fd except stdin (0). */
ssize_t harness_read(int fd, void *buf, size_t count)
{
    if (fd == 0) {
        if (count == 0) return 0;
        uint8_t byte;
        if (!harness_consume_kbd_byte(&byte))
            return 0;   /* no key available */
        ((uint8_t *)buf)[0] = byte;
        return 1;
    }
    return read(fd, buf, count);
}

/* time(): always returns epoch 0. */
time_t harness_time(time_t *t)
{
    if (t) *t = 0;
    return 0;
}

/* ftime(): fill struct with zeros. */
int harness_ftime(struct timeb *tp)
{
    if (tp) {
        tp->time = 0;
        tp->millitm = 0;
        tp->timezone = 0;
        tp->dstflag = 0;
    }
    return 0;
}

/* localtime(): return pointer to a zero-filled static struct tm. */
struct tm *harness_localtime(const time_t *t)
{
    static struct tm zero_tm;
    (void)t;
    zero_tm.tm_sec = 0;
    zero_tm.tm_min = 0;
    zero_tm.tm_hour = 0;
    zero_tm.tm_mday = 0;
    zero_tm.tm_mon = 0;
    zero_tm.tm_year = 0;
    zero_tm.tm_wday = 0;
    zero_tm.tm_yday = 0;
    zero_tm.tm_isdst = 0;
    return &zero_tm;
}
