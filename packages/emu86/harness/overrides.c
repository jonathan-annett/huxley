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

#include <time.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <unistd.h>

/* read(): forward to real libc read() for any fd except stdin (0). */
ssize_t harness_read(int fd, void *buf, size_t count)
{
    if (fd == 0)
        return 0;   /* no keyboard input in harness mode */
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
