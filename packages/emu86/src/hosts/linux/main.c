#define _POSIX_C_SOURCE 200809L
#include "run.h"
#include "state.h"
#include "platform.h"
#include "tables.h"
#include "snapshot.h"
#include "decode.h"
#include "platform_linux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>

#define CYCLE_BUDGET 20000
#define RING_BUF_SIZE 4096  /* must be power of 2 */

static Emu86State *state;
static Emu86Tables tables;
static Emu86Platform platform;
static DiskImage disks[MAX_DISKS];
static const char *snapshot_out_path = NULL;
static volatile int running = 1;

static void cleanup(void)
{
    terminal_restore();
}

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void save_snapshot(const char *path)
{
    uint32_t size = emu86_snapshot_size();
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { fprintf(stderr, "Failed to allocate snapshot buffer\n"); return; }
    uint32_t written = emu86_snapshot_save(state, buf, size);
    if (written > 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(buf, 1, written, f);
            fclose(f);
            fprintf(stderr, "\nSnapshot saved to %s (%u bytes)\n", path, written);
        } else {
            fprintf(stderr, "\nFailed to open %s for writing\n", path);
        }
    }
    free(buf);
}

static int load_snapshot(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open snapshot %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, size, f);
    fclose(f);
    int rc = emu86_snapshot_restore(state, buf, (uint32_t)size);
    free(buf);
    if (rc != 0)
        fprintf(stderr, "Snapshot restore failed (error %d)\n", rc);
    return rc;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <bios> <floppy> [@hd] [--snapshot-in file] [--snapshot-out file]\n", prog);
    fprintf(stderr, "  Prefix HD path with @ to boot from hard disk\n");
}

int main(int argc, char **argv)
{
    const char *bios_path = NULL;
    const char *fd_path = NULL;
    const char *hd_path = NULL;
    const char *snapshot_in_path = NULL;
    int boot_hd = 0;

    /* Parse arguments */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--snapshot-in") == 0 && i + 1 < argc) {
            snapshot_in_path = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-out") == 0 && i + 1 < argc) {
            snapshot_out_path = argv[++i];
        } else if (!bios_path) {
            bios_path = argv[i];
        } else if (!fd_path) {
            fd_path = argv[i];
        } else if (!hd_path) {
            hd_path = argv[i];
            if (hd_path[0] == '@') {
                hd_path++;
                boot_hd = 1;
            }
        }
        i++;
    }

    if (!bios_path || !fd_path) {
        usage(argv[0]);
        return 1;
    }

    /* Allocate state on the heap (~1.1 MB) */
    state = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!state) {
        fprintf(stderr, "Failed to allocate emulator state\n");
        return 1;
    }
    emu86_init(state);

    /* Open disk images: match original order — disk[0]=HD, disk[1]=FD, disk[2]=BIOS */
    memset(disks, 0, sizeof(disks));

    /* BIOS (disk[2]) */
    disks[2].fd = open(bios_path, O_RDONLY);
    if (disks[2].fd < 0) {
        fprintf(stderr, "Cannot open BIOS: %s\n", bios_path);
        return 1;
    }

    /* Floppy (disk[1]) */
    disks[1].fd = open(fd_path, O_RDWR);
    if (disks[1].fd < 0) {
        fprintf(stderr, "Cannot open floppy: %s\n", fd_path);
        return 1;
    }
    disks[1].size = (uint32_t)lseek(disks[1].fd, 0, SEEK_END);
    lseek(disks[1].fd, 0, SEEK_SET);

    /* Hard disk (disk[0]) — optional */
    if (hd_path) {
        disks[0].fd = open(hd_path, O_RDWR);
        if (disks[0].fd < 0)
            fprintf(stderr, "Warning: Cannot open HD: %s\n", hd_path);
        else {
            disks[0].size = (uint32_t)lseek(disks[0].fd, 0, SEEK_END);
            lseek(disks[0].fd, 0, SEEK_SET);
        }
    }

    /* Set boot drive: DL = 0 for FD, 0x80 for HD */
    state->regs[REG_DX] = boot_hd ? 0x80 : 0x00; /* DL */

    /* Set HD size in CX:AX (in sectors) */
    if (disks[0].fd > 0) {
        uint32_t sectors = disks[0].size >> 9;
        state->regs[REG_AX] = (uint16_t)sectors;
        state->regs[REG_CX] = (uint16_t)(sectors >> 16);
    }

    /* Load BIOS into F000:0100 */
    ssize_t bios_n = read(disks[2].fd, state->mem + 0xF0100, 0xFF00);
    if (bios_n <= 0) {
        fprintf(stderr, "Failed to read BIOS\n");
        return 1;
    }

    /* Load decode tables from BIOS */
    emu86_load_tables(&tables, state);

    /* Set up disk geometry in state */
    for (int d = 0; d < MAX_DISKS; d++) {
        state->disk[d].size = disks[d].size;
    }

    /* Load snapshot if requested (overrides init state) */
    if (snapshot_in_path) {
        if (load_snapshot(snapshot_in_path) != 0)
            return 1;
        /* Reload tables since memory may have changed */
        emu86_load_tables(&tables, state);
    }

    /* Set up platform */
    memset(&platform, 0, sizeof(platform));
    platform.disk_read = platform_disk_read;
    platform.disk_write = platform_disk_write;
    platform.get_time_us = platform_get_time_us;
    platform.ctx = disks;  /* pass disk array as context */

    alloc_ring_buffer(&platform.console_in, RING_BUF_SIZE);
    alloc_ring_buffer(&platform.console_out, RING_BUF_SIZE);

    /* Register cleanup */
    atexit(cleanup);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Set terminal to raw mode */
    terminal_init();

    /* Main loop */
    Emu86YieldInfo yield;
    while (running) {
        emu86_run(state, &platform, &tables, CYCLE_BUDGET, &yield);

        switch (yield.reason) {
        case EMU86_YIELD_BUDGET:
            flush_console(&platform);
            service_keyboard(&platform);
            break;

        case EMU86_YIELD_IO_NEEDED:
            flush_console(&platform);
            break;

        case EMU86_YIELD_HALTED:
            flush_console(&platform);
            {
                /* Wait for input or timer with select() */
                fd_set fds;
                struct timeval tv = { 0, 10000 }; /* 10ms */
                FD_ZERO(&fds);
                FD_SET(STDIN_FILENO, &fds);
                if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
                    service_keyboard(&platform);
                state->halted = 0;
                state->int_pending = 1;
                state->int_vector = 0x0A; /* timer */
            }
            break;

        case EMU86_YIELD_EXIT:
            running = 0;
            break;

        case EMU86_YIELD_ERROR:
            terminal_restore();
            fprintf(stderr, "\nEmulator error at %04X:%04X\n",
                    state->sregs[SREG_CS], state->ip);
            running = 0;
            break;
        }
    }

    /* Save snapshot if requested */
    if (snapshot_out_path)
        save_snapshot(snapshot_out_path);

    /* Cleanup */
    terminal_restore();
    flush_console(&platform);
    free_ring_buffer(&platform.console_in);
    free_ring_buffer(&platform.console_out);
    for (int d = 0; d < MAX_DISKS; d++) {
        if (disks[d].fd > 0)
            close(disks[d].fd);
    }
    free(state);

    return 0;
}
