#ifndef EMU86_PLATFORM_LINUX_H
#define EMU86_PLATFORM_LINUX_H

#include "state.h"
#include "platform.h"
#include <stdint.h>

/* --- Terminal --- */
void terminal_init(void);
void terminal_restore(void);
int  terminal_read(void);  /* non-blocking, returns -1 if no data */

/* --- Disk --- */
typedef struct {
    int      fd;
    uint32_t size;
} DiskImage;

#define MAX_DISKS 3  /* 0=HD, 1=FD, 2=BIOS */

int  platform_disk_read(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx);
int  platform_disk_write(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx);

/* --- Timer --- */
uint64_t platform_get_time_us(void *ctx);

/* --- Console service --- */
void service_keyboard(Emu86Platform *p);
void flush_console(Emu86Platform *p);

/* --- Ring buffer helpers --- */
void alloc_ring_buffer(Emu86RingBuf *rb, uint32_t size);
void free_ring_buffer(Emu86RingBuf *rb);

#endif /* EMU86_PLATFORM_LINUX_H */
