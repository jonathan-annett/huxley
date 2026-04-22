/*
 * harness.c — differential test harness (EMU-16)
 *
 * Runs our emulator in lockstep with the reference (8086tiny.c). After
 * every reference instruction, executes one of ours, compares CPU +
 * machine state, halts on the first divergence with a detailed report.
 *
 * Called from reference/8086tiny.c via two macros (see EMU-16 task):
 *   HARNESS_STEP_BEGIN() — top of each sim() iteration (cold-start init
 *                          on first call)
 *   HARNESS_STEP_END()   — end of each sim() iteration (advance our
 *                          emulator, compare, halt on divergence)
 *
 * Scope v1: cold-boot lockstep with empty keyboard input and zeroed RTC.
 * Divergence is REPORTED, not fixed — see tasks/emu16-task.md.
 */

#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "run.h"
#include "platform.h"
#include "tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>

/* Scratch directory for per-emulator disk copies (EMU-17). Each run overwrites
 * fresh copies; files are preserved after exit for offline diff inspection. */
#define HARNESS_SCRATCH_DIR "/tmp/emu86-harness"
#define HARNESS_HEARTBEAT_PATH HARNESS_SCRATCH_DIR "/heartbeat.log"

/* ================================================================
 * Reference globals (declared in reference/8086tiny.c)
 * ================================================================ */

#define REF_RAM_SIZE      0x10FFF0
#define REF_IO_PORT_COUNT 0x10000
#define REF_REGS_BASE     0xF0000

extern unsigned char  mem[REF_RAM_SIZE];
extern unsigned char  io_ports[REF_IO_PORT_COUNT];
extern unsigned short *regs16;
extern unsigned char  *regs8;
extern unsigned short reg_ip;
extern unsigned int   inst_counter;
extern unsigned char  int8_asap;
extern unsigned char  trap_flag;
extern unsigned char  seg_override_en;
extern unsigned short seg_override;
extern unsigned char  rep_override_en;
extern unsigned char  rep_mode;
extern unsigned char  spkr_en;
extern unsigned char  io_hi_lo;
extern int            disk[3];
extern unsigned char  bios_table_lookup[20][256];

/* Reference flag byte offsets inside regs8[] — see 8086tiny.c FLAG_xx defines */
#define REF_FLAG_CF 40
#define REF_FLAG_PF 41
#define REF_FLAG_AF 42
#define REF_FLAG_ZF 43
#define REF_FLAG_SF 44
#define REF_FLAG_TF 45
#define REF_FLAG_IF 46
#define REF_FLAG_DF 47
#define REF_FLAG_OF 48

/* Reference register indices (in regs16[]) */
#define REF_REG_AX 0
#define REF_REG_CX 1
#define REF_REG_DX 2
#define REF_REG_BX 3
#define REF_REG_SP 4
#define REF_REG_BP 5
#define REF_REG_SI 6
#define REF_REG_DI 7
#define REF_REG_ES 8
#define REF_REG_CS 9
#define REF_REG_SS 10
#define REF_REG_DS 11

/* ================================================================
 * Our-side state (heap-allocated on first use)
 * ================================================================ */

static Emu86State  *our_state;
static Emu86Tables  our_tables;
static Emu86Platform our_platform;

/* Disk image descriptors used by our platform_disk_read/write.
 * We borrow the reference's fds rather than opening new ones so both
 * sides share the file offset state. */
typedef struct {
    int      fd;
    uint32_t size;
} DiskImage;

#define MAX_DISKS 3
static DiskImage our_disks[MAX_DISKS];

static int       harness_initialised;
static uint64_t  harness_step_count;
static uint64_t  harness_step_limit = 200000000ULL; /* default: very generous */
static int       harness_inject_divergence = 0;     /* test hook */
static uint64_t  harness_inject_at = 0;
static int       harness_selftest_mode = 0;         /* compare ref↔ref */

/* EMU-24: heartbeat log — fine-grained progress visibility for long runs
 * and hang diagnosis. Written to /tmp/emu86-harness/heartbeat.log, truncated
 * on startup, flushed after every line. */
static FILE     *harness_heartbeat_fp;
static uint64_t  harness_heartbeat_every = 1000;
static uint64_t  harness_last_heartbeat_us;

/* EMU-17: hux-side scratch paths, populated by main() before reference_main()
 * is called. harness_init opens these with O_RDWR on our side so that neither
 * emulator touches the source disk image. Empty string = not provided. */
static char      hux_floppy_path[PATH_MAX];
static char      hux_hd_path[PATH_MAX];

/* EMU-31: split-output mode. When HARNESS_SPLIT_OUTPUT=1, reference fd 1 and
 * our emulator's console_out ringbuf each go to a named pipe in the scratch
 * dir, so both streams can be observed in parallel (e.g. cat on two terminals).
 * Default off: ref writes to terminal, our ringbuf drains silently, as before. */
static int       harness_split_output;
static int       ref_fifo_fd = -1;
static int       hux_fifo_fd = -1;

/* ================================================================
 * Platform callbacks for our emulator
 * ================================================================ */

static int
our_disk_read(int drive, uint32_t offset, uint8_t *buf, uint32_t len, void *ctx)
{
    DiskImage *d = (DiskImage *)ctx;
    if (drive < 0 || drive >= MAX_DISKS || d[drive].fd <= 0)
        return -1;
    if (lseek(d[drive].fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = read(d[drive].fd, buf, len);
    return (n >= 0) ? 0 : -1;
}

static int
our_disk_write(int drive, uint32_t offset, const uint8_t *buf, uint32_t len, void *ctx)
{
    DiskImage *d = (DiskImage *)ctx;
    if (drive < 0 || drive >= MAX_DISKS || d[drive].fd <= 0)
        return -1;
    if (lseek(d[drive].fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = write(d[drive].fd, buf, len);
    return (n >= 0) ? 0 : -1;
}

/* Deterministic time: always zero. (Our side; reference side is neutralised
 * by -Dtime=harness_time etc.) */
static uint64_t our_get_time_us(void *ctx) { (void)ctx; return 0; }

/* Ring buffer storage for console I/O — we allocate but nothing reads
 * console_out, and nothing writes to console_in (no key input). */
#define RB_SIZE 4096
static uint8_t  rb_out_buf[RB_SIZE];
static uint8_t  rb_in_buf[RB_SIZE];
static volatile uint32_t rb_out_head, rb_out_tail, rb_in_head, rb_in_tail;

/* ================================================================
 * Reference flag packing (ref stores flags as individual bytes)
 * ================================================================ */

static uint16_t pack_ref_flags(void)
{
    uint16_t f = 0;
    if (regs8[REF_FLAG_CF]) f |= FLAG_CF;
    if (regs8[REF_FLAG_PF]) f |= FLAG_PF;
    if (regs8[REF_FLAG_AF]) f |= FLAG_AF;
    if (regs8[REF_FLAG_ZF]) f |= FLAG_ZF;
    if (regs8[REF_FLAG_SF]) f |= FLAG_SF;
    if (regs8[REF_FLAG_TF]) f |= FLAG_TF;
    if (regs8[REF_FLAG_IF]) f |= FLAG_IF;
    if (regs8[REF_FLAG_DF]) f |= FLAG_DF;
    if (regs8[REF_FLAG_OF]) f |= FLAG_OF;
    return f;
}

/* Reference seg_override holds 8..11 (ES,CS,SS,DS original numbering).
 * Our seg_override holds 0..3 (SREG_xx). Convert when comparing. */
static uint8_t ref_to_ours_sreg(uint16_t ref_sreg)
{
    if (ref_sreg >= 8 && ref_sreg <= 11)
        return (uint8_t)(ref_sreg - 8);
    return 0xFF; /* invalid */
}

/* ================================================================
 * Initialisation — called on first HARNESS_STEP_BEGIN
 *
 * At this point the reference has:
 *   - opened disks (fds in disk[0..2])
 *   - loaded the BIOS at mem[0xF0100]
 *   - set up regs16 pointing to mem+0xF0000
 *   - set CS=0xF000, IP=0x0100, DL=boot drive, possibly CX:AX=HD size
 *   - loaded bios_table_lookup
 *
 * We clone all of this into our state.
 * ================================================================ */

static void harness_init(void)
{
    our_state = (Emu86State *)calloc(1, sizeof(Emu86State));
    if (!our_state) {
        fprintf(stderr, "harness: calloc Emu86State (%zu bytes) failed\n",
                sizeof(Emu86State));
        exit(1);
    }
    emu86_init(our_state);

    /* Copy memory from the reference.
     * The ref keeps registers in mem[0xF0000..0xF00FF]; we have registers
     * in struct fields, so we copy [0..0xF0000) and [0xF0100..end).
     * That leaves mem[0xF0000..0xF00FF] unused on our side (matches the
     * reference's layout by *not* mirroring the register-mapped region). */
    size_t prefix_size = REF_REGS_BASE; /* 0xF0000 */
    size_t bios_size   = REF_RAM_SIZE - (REF_REGS_BASE + 0x100);
    memcpy(our_state->mem, mem, prefix_size);
    memcpy(our_state->mem + REF_REGS_BASE + 0x100,
           mem + REF_REGS_BASE + 0x100, bios_size);

    /* I/O ports */
    memcpy(our_state->io_ports, io_ports, REF_IO_PORT_COUNT);

    /* Registers */
    for (int i = 0; i < 8; i++)
        our_state->regs[i] = regs16[i];                /* AX..DI */
    for (int i = 0; i < 4; i++)
        our_state->sregs[i] = regs16[REF_REG_ES + i];  /* ES,CS,SS,DS */
    our_state->ip = reg_ip;
    our_state->flags = pack_ref_flags();

    /* Prefix state */
    our_state->seg_override_en = seg_override_en;
    uint8_t ours_so = ref_to_ours_sreg(seg_override);
    our_state->seg_override = (ours_so == 0xFF) ? 0 : ours_so;
    our_state->rep_override_en = rep_override_en;
    our_state->rep_mode = rep_mode;

    /* Trap flag */
    our_state->trap_flag = trap_flag;
    our_state->int8_asap = int8_asap;
    our_state->spkr_en = spkr_en;
    our_state->pit_lobyte_pending = io_hi_lo;

    /* Load decode tables from our mem (which has the BIOS). */
    emu86_load_tables(&our_tables, our_state);

    /* Cross-check table load: compare against ref's bios_table_lookup. */
    if (memcmp(our_tables.data, bios_table_lookup,
               sizeof(our_tables.data)) != 0) {
        fprintf(stderr, "harness: decode tables mismatch at init!\n");
        exit(1);
    }

    /* EMU-17: open our own fds against the hux scratch copies. The reference
     * has already opened its own .ref.scratch fds via its normal argv path;
     * we open separate fds on the .hux.scratch files so neither side ever
     * touches the source image and divergent writes land in separate files.
     *
     * BIOS (disk[2]) is read-only and is consumed by the reference at init
     * time to load mem[0xF0100]; our side reads BIOS out of mem, not disk.
     * So we share the reference's BIOS fd (it's never written by either side). */
    our_disks[2].fd = disk[2];
    off_t bios_sz = (disk[2] > 0) ? lseek(disk[2], 0, SEEK_END) : -1;
    our_disks[2].size = (bios_sz > 0) ? (uint32_t)bios_sz : 0;

    if (hux_floppy_path[0]) {
        our_disks[1].fd = open(hux_floppy_path, O_RDWR);
        if (our_disks[1].fd < 0) {
            fprintf(stderr, "harness: cannot open hux floppy scratch '%s': %s\n",
                    hux_floppy_path, strerror(errno));
            exit(1);
        }
        off_t sz = lseek(our_disks[1].fd, 0, SEEK_END);
        our_disks[1].size = (sz > 0) ? (uint32_t)sz : 0;
    } else {
        our_disks[1].fd = 0;
        our_disks[1].size = 0;
    }

    if (hux_hd_path[0]) {
        our_disks[0].fd = open(hux_hd_path, O_RDWR);
        if (our_disks[0].fd < 0) {
            fprintf(stderr, "harness: cannot open hux HD scratch '%s': %s\n",
                    hux_hd_path, strerror(errno));
            exit(1);
        }
        off_t sz = lseek(our_disks[0].fd, 0, SEEK_END);
        our_disks[0].size = (sz > 0) ? (uint32_t)sz : 0;
    } else {
        our_disks[0].fd = 0;
        our_disks[0].size = 0;
    }

    for (int i = 0; i < MAX_DISKS; i++)
        our_state->disk[i].size = our_disks[i].size;

    /* Platform setup */
    memset(&our_platform, 0, sizeof(our_platform));
    our_platform.disk_read  = our_disk_read;
    our_platform.disk_write = our_disk_write;
    our_platform.get_time_us = our_get_time_us;
    our_platform.ctx = our_disks;

    /* Console ring buffers (only to keep emu86_run happy; nothing observes them). */
    rb_out_head = rb_out_tail = rb_in_head = rb_in_tail = 0;
    our_platform.console_out.buf = rb_out_buf;
    our_platform.console_out.size = RB_SIZE;
    our_platform.console_out.head = &rb_out_head;
    our_platform.console_out.tail = &rb_out_tail;
    our_platform.console_in.buf = rb_in_buf;
    our_platform.console_in.size = RB_SIZE;
    our_platform.console_in.head = &rb_in_head;
    our_platform.console_in.tail = &rb_in_tail;

    /* Parse environment overrides */
    const char *limit_env = getenv("HARNESS_STEP_LIMIT");
    if (limit_env) {
        char *end;
        unsigned long long v = strtoull(limit_env, &end, 0);
        if (*end == '\0') harness_step_limit = (uint64_t)v;
    }
    const char *inj_env = getenv("HARNESS_INJECT_DIVERGENCE_AT");
    if (inj_env) {
        char *end;
        unsigned long long v = strtoull(inj_env, &end, 0);
        if (*end == '\0') {
            harness_inject_divergence = 1;
            harness_inject_at = (uint64_t)v;
        }
    }
    if (getenv("HARNESS_SELFTEST")) {
        harness_selftest_mode = 1;
    }

    harness_initialised = 1;

    fprintf(stderr, "harness: initialised at CS:IP=%04X:%04X "
                    "(step limit=%llu, selftest=%d, inject_at=%llu)\n",
            regs16[REF_REG_CS], reg_ip,
            (unsigned long long)harness_step_limit,
            harness_selftest_mode,
            harness_inject_divergence
                ? (unsigned long long)harness_inject_at : 0ULL);
    fflush(stderr);
}

/* ================================================================
 * EMU-28: Pre-step state snapshot with rotating buffers
 *
 * At each clean-step boundary we want to remember the state going
 * into the *next* instruction, so that if the next step diverges
 * the report can show the pre-step state alongside the post-step
 * state the comparator already reports.
 *
 * Because "state after step N" equals "state before step N+1", we
 * only populate one buffer per side per step and rotate which
 * buffer plays the "pre" role: at end of clean step N, populate
 * `next_*` with current live state, then swap `pre_*` ↔ `next_*`
 * so the newly-populated buffer becomes "pre" for step N+1.
 *
 * If step N+1 diverges, `pre_*` still holds state-after-N (=
 * state-before-N+1) as populated at the end of step N. The old
 * pre-buffer is now the scratch that would have been overwritten
 * in step N+1's tail — which never runs because we're halting.
 *
 * The buffers capture registers, segment registers, IP, flags,
 * interrupt/prefix status bytes and the 8 opcode bytes at CS:IP.
 * Not memory, not io_ports — those are large and live state is
 * available for memory/io divergence reporting directly.
 * ================================================================ */

typedef struct {
    uint16_t regs[8];        /* AX, CX, DX, BX, SP, BP, SI, DI (OUR indexing) */
    uint16_t sregs[4];       /* ES, CS, SS, DS (OUR indexing) */
    uint16_t ip;
    uint16_t flags;
    uint8_t  int8_asap;
    uint8_t  seg_override_en;
    uint8_t  rep_override_en;
    uint8_t  trap_flag;
    uint8_t  seg_override;   /* OUR sreg index (0-3), or 0xFF if none */
    uint8_t  rep_mode;
    uint8_t  opcode_bytes[8];
    uint32_t inst_count;
} StepSnapshot;

static StepSnapshot snap_ref_a, snap_ref_b;
static StepSnapshot snap_our_a, snap_our_b;
static StepSnapshot *pre_ref  = &snap_ref_a;
static StepSnapshot *next_ref = &snap_ref_b;
static StepSnapshot *pre_our  = &snap_our_a;
static StepSnapshot *next_our = &snap_our_b;

static void populate_ref_snapshot(StepSnapshot *s)
{
    for (int i = 0; i < 8; i++)
        s->regs[i] = regs16[i];
    for (int i = 0; i < 4; i++)
        s->sregs[i] = regs16[REF_REG_ES + i];
    s->ip = reg_ip;
    s->flags = pack_ref_flags();
    s->int8_asap = int8_asap;
    s->seg_override_en = seg_override_en;
    s->rep_override_en = rep_override_en;
    s->trap_flag = trap_flag;
    s->seg_override = ref_to_ours_sreg(seg_override);
    s->rep_mode = rep_mode;
    s->inst_count = inst_counter;
    uint32_t lin = ((uint32_t)regs16[REF_REG_CS] << 4) + reg_ip;
    for (int i = 0; i < 8; i++)
        s->opcode_bytes[i] = (lin + i < REF_RAM_SIZE) ? mem[lin + i] : 0;
}

static void populate_our_snapshot(StepSnapshot *s)
{
    for (int i = 0; i < 8; i++)
        s->regs[i] = our_state->regs[i];
    for (int i = 0; i < 4; i++)
        s->sregs[i] = our_state->sregs[i];
    s->ip = our_state->ip;
    s->flags = our_state->flags;
    s->int8_asap = our_state->int8_asap;
    s->seg_override_en = our_state->seg_override_en;
    s->rep_override_en = our_state->rep_override_en;
    s->trap_flag = our_state->trap_flag;
    s->seg_override = our_state->seg_override;
    s->rep_mode = our_state->rep_mode;
    s->inst_count = (uint32_t)our_state->inst_count;
    uint32_t lin = ((uint32_t)our_state->sregs[SREG_CS] << 4) + our_state->ip;
    for (int i = 0; i < 8; i++)
        s->opcode_bytes[i] = (lin + i < EMU86_MEM_SIZE) ? our_state->mem[lin + i] : 0;
}

static void rotate_snapshots(void)
{
    StepSnapshot *tmp = pre_ref; pre_ref = next_ref; next_ref = tmp;
    tmp = pre_our; pre_our = next_our; next_our = tmp;
}

/* ================================================================
 * State comparison
 *
 * Populates the per-call `diverge_*` fields; returns 0 if identical,
 * otherwise a bit-set describing which categories differ.
 * ================================================================ */

#define DIV_IP           0x00000001
#define DIV_REGS         0x00000002
#define DIV_SREGS        0x00000004
#define DIV_FLAGS        0x00000008
#define DIV_PREFIX       0x00000010
#define DIV_TRAP_FLAG    0x00000020
#define DIV_INT8_ASAP    0x00000040
#define DIV_INST_COUNT   0x00000080
#define DIV_MEM          0x00000100
#define DIV_IO_PORTS     0x00000200
#define DIV_SPKR_EN      0x00000400
#define DIV_IO_HI_LO     0x00000800

static uint32_t
compare_states(uint32_t *mem_diff_addr, uint32_t *io_diff_addr,
               uint32_t *reg_diff_idx)
{
    uint32_t flags = 0;
    *mem_diff_addr = 0xFFFFFFFF;
    *io_diff_addr = 0xFFFFFFFF;
    *reg_diff_idx = 0xFFFFFFFF;

    if (our_state->ip != reg_ip)
        flags |= DIV_IP;

    for (int i = 0; i < 8; i++) {
        if (our_state->regs[i] != regs16[i]) {
            flags |= DIV_REGS;
            if (*reg_diff_idx == 0xFFFFFFFF) *reg_diff_idx = (uint32_t)i;
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (our_state->sregs[i] != regs16[REF_REG_ES + i]) {
            flags |= DIV_SREGS;
            break;
        }
    }

    uint16_t rf = pack_ref_flags();
    uint16_t of = our_state->flags & 0x0FD5; /* mask to defined bits */
    if (rf != of)
        flags |= DIV_FLAGS;

    /* REG_ZERO (12) is the reference's sentinel for LEA's offset-
     * computation trick. It has no guest-observable meaning — the
     * corresponding seg_override_en is transient and decrements to 0
     * before any instruction consumes it. Our emulator has no
     * equivalent sentinel (and doesn't need one, since we compute LEA
     * offsets directly). Treat REG_ZERO as "don't compare" for the
     * seg_override field. See docs/notes/8086tiny-quirks.md. */
    #define REG_ZERO 12
    uint8_t ref_so = ref_to_ours_sreg(seg_override);
    int seg_override_differs =
        seg_override_en
        && seg_override != REG_ZERO
        && our_state->seg_override != ref_so;

    if (our_state->seg_override_en != seg_override_en ||
        seg_override_differs ||
        our_state->rep_override_en != rep_override_en ||
        (rep_override_en && our_state->rep_mode != rep_mode))
        flags |= DIV_PREFIX;

    if (our_state->trap_flag != trap_flag)
        flags |= DIV_TRAP_FLAG;

    if (our_state->int8_asap != int8_asap)
        flags |= DIV_INT8_ASAP;

    if ((uint32_t)our_state->inst_count != inst_counter)
        flags |= DIV_INST_COUNT;

    if (our_state->spkr_en != spkr_en)
        flags |= DIV_SPKR_EN;

    if (our_state->pit_lobyte_pending != io_hi_lo)
        flags |= DIV_IO_HI_LO;

    /* Memory: compare [0, 0xF0000) then [0xF0100, end) */
    for (uint32_t a = 0; a < REF_REGS_BASE; a++) {
        if (our_state->mem[a] != mem[a]) {
            flags |= DIV_MEM;
            *mem_diff_addr = a;
            break;
        }
    }
    if (!(flags & DIV_MEM)) {
        uint32_t start = REF_REGS_BASE + 0x100;
        for (uint32_t a = start; a < REF_RAM_SIZE; a++) {
            if (our_state->mem[a] != mem[a]) {
                flags |= DIV_MEM;
                *mem_diff_addr = a;
                break;
            }
        }
    }

    /* I/O ports */
    for (uint32_t p = 0; p < REF_IO_PORT_COUNT; p++) {
        if (our_state->io_ports[p] != io_ports[p]) {
            flags |= DIV_IO_PORTS;
            *io_diff_addr = p;
            break;
        }
    }

    return flags;
}

/* ================================================================
 * Divergence reporting
 * ================================================================ */

static void
report_divergence(uint32_t dflags, uint32_t mem_addr, uint32_t io_addr,
                  uint32_t reg_idx)
{
    static const char *reg_names[8] = {"AX","CX","DX","BX","SP","BP","SI","DI"};
    static const char *sreg_names[4] = {"ES","CS","SS","DS"};

    fprintf(stderr, "\n\n======== HARNESS DIVERGENCE ========\n");
    fprintf(stderr, "Step             : %llu\n",
            (unsigned long long)harness_step_count);

    /* Pre-step state (populated at end of previous clean step).
     * Both sides agreed here — otherwise a prior step would have
     * diverged — so we print it once. This is the state going
     * into the instruction that just diverged. Printed first so
     * it's visible without scrolling past memory diffs. */
    unsigned long long prev_step =
        (harness_step_count > 0) ? harness_step_count - 1 : 0;
    fprintf(stderr, "\n-- Pre-step state (end of step %llu, "
                    "input to step %llu) --\n",
            prev_step, (unsigned long long)harness_step_count);
    fprintf(stderr, "CS:IP            : %04X:%04X\n",
            pre_ref->sregs[SREG_CS], pre_ref->ip);
    fprintf(stderr, "Opcode bytes     : ");
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "%02X ", pre_ref->opcode_bytes[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "Registers        : "
                    "AX=%04X CX=%04X DX=%04X BX=%04X "
                    "SP=%04X BP=%04X SI=%04X DI=%04X\n",
            pre_ref->regs[0], pre_ref->regs[1], pre_ref->regs[2],
            pre_ref->regs[3], pre_ref->regs[4], pre_ref->regs[5],
            pre_ref->regs[6], pre_ref->regs[7]);
    fprintf(stderr, "Segment regs     : ES=%04X CS=%04X SS=%04X DS=%04X\n",
            pre_ref->sregs[0], pre_ref->sregs[1],
            pre_ref->sregs[2], pre_ref->sregs[3]);
    fprintf(stderr, "FLAGS            : %04X\n", pre_ref->flags);
    fprintf(stderr, "Status           : int8_asap=%u seg_ovr_en=%u "
                    "seg_ovr=%u rep_ovr_en=%u TF=%u\n",
            pre_ref->int8_asap, pre_ref->seg_override_en,
            pre_ref->seg_override,
            pre_ref->rep_override_en, pre_ref->trap_flag);

    fprintf(stderr, "\n-- Post-step divergence (after step %llu executed) --\n",
            (unsigned long long)harness_step_count);
    fprintf(stderr, "Ref CS:IP        : %04X:%04X\n",
            regs16[REF_REG_CS], reg_ip);
    fprintf(stderr, "Our CS:IP        : %04X:%04X\n",
            our_state->sregs[SREG_CS], our_state->ip);

    uint32_t lin = ((uint32_t)regs16[REF_REG_CS] << 4) + reg_ip;
    fprintf(stderr, "Ref opcode bytes : ");
    for (int i = 0; i < 8 && lin + i < REF_RAM_SIZE; i++)
        fprintf(stderr, "%02X ", mem[lin + i]);
    fprintf(stderr, "\n");

    uint32_t our_lin = ((uint32_t)our_state->sregs[SREG_CS] << 4)
                     + our_state->ip;
    fprintf(stderr, "Our opcode bytes : ");
    for (int i = 0; i < 8 && our_lin + i < EMU86_MEM_SIZE; i++)
        fprintf(stderr, "%02X ", our_state->mem[our_lin + i]);
    fprintf(stderr, "\n");

    fprintf(stderr, "Categories       : 0x%08X\n", dflags);

    if (dflags & DIV_IP)
        fprintf(stderr, "  IP differs: ref=%04X ours=%04X\n",
                reg_ip, our_state->ip);

    if (dflags & DIV_REGS) {
        fprintf(stderr, "  GP registers:\n");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, "    %s: ref=%04X ours=%04X%s\n",
                    reg_names[i], regs16[i], our_state->regs[i],
                    regs16[i] != our_state->regs[i] ? "  <<" : "");
    }
    if (dflags & DIV_SREGS) {
        fprintf(stderr, "  Segment registers:\n");
        for (int i = 0; i < 4; i++)
            fprintf(stderr, "    %s: ref=%04X ours=%04X%s\n",
                    sreg_names[i], regs16[REF_REG_ES + i],
                    our_state->sregs[i],
                    regs16[REF_REG_ES + i] != our_state->sregs[i] ? "  <<" : "");
    }
    if (dflags & DIV_FLAGS) {
        uint16_t rf = pack_ref_flags();
        uint16_t of = our_state->flags & 0x0FD5;
        fprintf(stderr, "  FLAGS: ref=%04X ours=%04X  xor=%04X\n",
                rf, of, rf ^ of);
        static const struct { const char *n; uint16_t m; } fmap[] = {
            {"CF",FLAG_CF},{"PF",FLAG_PF},{"AF",FLAG_AF},{"ZF",FLAG_ZF},
            {"SF",FLAG_SF},{"TF",FLAG_TF},{"IF",FLAG_IF},{"DF",FLAG_DF},
            {"OF",FLAG_OF},
        };
        for (unsigned i = 0; i < sizeof(fmap)/sizeof(fmap[0]); i++)
            if ((rf & fmap[i].m) != (of & fmap[i].m))
                fprintf(stderr, "    %s: ref=%d ours=%d\n",
                        fmap[i].n, !!(rf & fmap[i].m), !!(of & fmap[i].m));
    }
    if (dflags & DIV_PREFIX) {
        fprintf(stderr, "  Prefix state:\n");
        fprintf(stderr, "    seg_override_en: ref=%u ours=%u\n",
                seg_override_en, our_state->seg_override_en);
        fprintf(stderr, "    seg_override:    ref=%u (→%d) ours=%u\n",
                seg_override, ref_to_ours_sreg(seg_override),
                our_state->seg_override);
        fprintf(stderr, "    rep_override_en: ref=%u ours=%u\n",
                rep_override_en, our_state->rep_override_en);
        fprintf(stderr, "    rep_mode:        ref=%u ours=%u\n",
                rep_mode, our_state->rep_mode);
    }
    if (dflags & DIV_TRAP_FLAG)
        fprintf(stderr, "  trap_flag: ref=%u ours=%u\n",
                trap_flag, our_state->trap_flag);
    if (dflags & DIV_INT8_ASAP)
        fprintf(stderr, "  int8_asap: ref=%u ours=%u\n",
                int8_asap, our_state->int8_asap);
    if (dflags & DIV_INST_COUNT)
        fprintf(stderr, "  inst_count: ref=%u ours=%llu\n",
                inst_counter, (unsigned long long)our_state->inst_count);
    if (dflags & DIV_SPKR_EN)
        fprintf(stderr, "  spkr_en: ref=%u ours=%u\n",
                spkr_en, our_state->spkr_en);
    if (dflags & DIV_IO_HI_LO)
        fprintf(stderr, "  io_hi_lo: ref=%u ours=%u\n",
                io_hi_lo, our_state->pit_lobyte_pending);

    if (dflags & DIV_MEM) {
        fprintf(stderr, "  Memory: first diff at 0x%05X\n", mem_addr);
        uint32_t start = mem_addr > 8 ? mem_addr - 8 : 0;
        fprintf(stderr, "    [%05X] ref : ", start);
        for (int i = 0; i < 16 && start + i < REF_RAM_SIZE; i++)
            fprintf(stderr, "%02X%s", mem[start + i],
                    start + i == mem_addr ? "*" : " ");
        fprintf(stderr, "\n    [%05X] ours: ", start);
        for (int i = 0; i < 16 && start + i < EMU86_MEM_SIZE; i++)
            fprintf(stderr, "%02X%s", our_state->mem[start + i],
                    start + i == mem_addr ? "*" : " ");
        fprintf(stderr, "\n");
    }
    if (dflags & DIV_IO_PORTS) {
        fprintf(stderr, "  I/O ports: first diff at 0x%04X "
                        "(ref=%02X ours=%02X)\n",
                io_addr, io_ports[io_addr], our_state->io_ports[io_addr]);
    }
    fprintf(stderr, "====================================\n");
    fflush(stderr);
}

/* ================================================================
 * EMU-24: heartbeat log helpers
 * ================================================================ */

static uint64_t
harness_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void
harness_close_heartbeat(void)
{
    if (harness_heartbeat_fp) {
        fclose(harness_heartbeat_fp);
        harness_heartbeat_fp = NULL;
    }
}

static void
harness_emit_heartbeat(void)
{
    uint64_t now_us = harness_now_us();
    uint64_t dt_ms = (now_us - harness_last_heartbeat_us) / 1000ULL;

    uint32_t lin = ((uint32_t)regs16[REF_REG_CS] << 4) + reg_ip;
    fprintf(harness_heartbeat_fp,
            "step=%06llu cs=%04X ip=%04X dt_ms=%llu opcodes=",
            (unsigned long long)harness_step_count,
            regs16[REF_REG_CS], reg_ip,
            (unsigned long long)dt_ms);
    for (int i = 0; i < 8; i++) {
        uint8_t b = (lin + i < REF_RAM_SIZE) ? mem[lin + i] : 0;
        fprintf(harness_heartbeat_fp, "%s%02X", i ? " " : "", b);
    }
    fputc('\n', harness_heartbeat_fp);
    fflush(harness_heartbeat_fp);

    harness_last_heartbeat_us = now_us;
}

/* ================================================================
 * The two hooks called from sim()
 * ================================================================ */

void harness_step_begin(void)
{
    if (!harness_initialised)
        harness_init();
}

void harness_step_end(void)
{
    /* Self-test mode (HARNESS_SELFTEST=1): after each ref instruction,
     * snapshot ref → our_state, then call compare_states. A correctly
     * wired comparator must report zero divergence. Any non-zero result
     * here means the harness is misreading either side's state. */
    if (harness_selftest_mode) {
        for (int i = 0; i < 8; i++)
            our_state->regs[i] = regs16[i];
        for (int i = 0; i < 4; i++)
            our_state->sregs[i] = regs16[REF_REG_ES + i];
        our_state->ip = reg_ip;
        our_state->flags = pack_ref_flags();
        memcpy(our_state->mem, mem, REF_REGS_BASE);
        memcpy(our_state->mem + REF_REGS_BASE + 0x100,
               mem + REF_REGS_BASE + 0x100,
               REF_RAM_SIZE - REF_REGS_BASE - 0x100);
        memcpy(our_state->io_ports, io_ports, REF_IO_PORT_COUNT);
        our_state->seg_override_en = seg_override_en;
        uint8_t so = ref_to_ours_sreg(seg_override);
        our_state->seg_override = (so == 0xFF) ? 0 : so;
        our_state->rep_override_en = rep_override_en;
        our_state->rep_mode = rep_mode;
        our_state->trap_flag = trap_flag;
        our_state->int8_asap = int8_asap;
        our_state->spkr_en = spkr_en;
        our_state->pit_lobyte_pending = io_hi_lo;
        our_state->inst_count = inst_counter;

        uint32_t ma, ia, ri;
        uint32_t df = compare_states(&ma, &ia, &ri);
        if (df != 0) {
            fprintf(stderr, "SELFTEST FAIL: comparator reports divergence 0x%08X "
                            "after ref→our snapshot (should be 0)\n", df);
            report_divergence(df, ma, ia, ri);
            exit(3);
        }
        populate_ref_snapshot(next_ref);
        populate_our_snapshot(next_our);
        rotate_snapshots();
        harness_step_count++;
        if (harness_step_count >= harness_step_limit) {
            fprintf(stderr, "harness: SELFTEST PASS — %llu steps, "
                            "zero divergences after snapshot-compare each step.\n",
                    (unsigned long long)harness_step_count);
            fflush(stderr);
            exit(0);
        }
        return;
    }

    /* Step our emulator by exactly one instruction. */
    Emu86YieldInfo yi;
    emu86_run(our_state, &our_platform, &our_tables, 1, &yi);

    /* Drain console_out. Default: discard silently — the reference writes
     * to stdout directly, so if we also wrote we'd double the output. Our
     * state matches ref's observable bytes at the AL-write instant; the
     * byte in our ring buffer is therefore identical to ref's stdout byte.
     * EMU-31: under HARNESS_SPLIT_OUTPUT=1, forward each drained byte to
     * the hux fifo. Best-effort write — EPIPE/EAGAIN just drops the byte. */
    uint8_t ch;
    while (ringbuf_read(&our_platform.console_out, &ch) == 0) {
        if (harness_split_output) {
            ssize_t w = write(hux_fifo_fd, &ch, 1);
            (void)w;
        }
    }

    harness_step_count++;

    /* Optional: inject a deliberate divergence at a chosen step, to verify
     * the comparator detects it (Phase 4 self-test). */
    if (harness_inject_divergence && harness_step_count == harness_inject_at) {
        our_state->regs[REF_REG_AX] ^= 0xBEEF;
        fprintf(stderr, "harness: injected AX divergence at step %llu\n",
                (unsigned long long)harness_step_count);
    }

    /* Compare */
    uint32_t mem_addr, io_addr, reg_idx;
    uint32_t dflags = compare_states(&mem_addr, &io_addr, &reg_idx);

    /* EMU-25 debug: dump state at interesting step */
    if (harness_step_count == 65771 || harness_step_count == 65770) {
    fprintf(stderr, "\n=== DEBUG step %llu ===\n",
            (unsigned long long)harness_step_count);
    fprintf(stderr, "ref  CS:IP=%04X:%04X SP=%04X  mem[4A6]=%02X  int8_asap=%d IF=%d\n",
            regs16[REF_REG_CS], reg_ip, regs16[REF_REG_SP],
            mem[0x4A6], int8_asap, !!regs8[REF_FLAG_IF]);
    fprintf(stderr, "ours CS:IP=%04X:%04X SP=%04X  mem[4A6]=%02X  int8_asap=%d IF=%d\n",
            our_state->sregs[SREG_CS], our_state->ip, our_state->regs[REG_SP],
            our_state->mem[0x4A6], our_state->int8_asap,
            !!(our_state->flags & FLAG_IF));
    /* Stack dump - 16 bytes at ref SP */
    uint32_t sp_lin = ((uint32_t)regs16[REF_REG_SS] << 4) + regs16[REF_REG_SP];
    fprintf(stderr, "ref  stack [SS:SP..]: ");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02X ", mem[sp_lin + i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

    if (dflags) {
        report_divergence(dflags, mem_addr, io_addr, reg_idx);
        fprintf(stderr, "harness: halting at step %llu due to divergence.\n",
                (unsigned long long)harness_step_count);
        fflush(stderr);
        exit(2);
    }

    /* Clean step — populate the "next" snapshot buffers with current
     * live state, then rotate so "next" becomes "pre" for the step
     * we're about to execute. */
    populate_ref_snapshot(next_ref);
    populate_our_snapshot(next_our);
    rotate_snapshots();

    if (harness_step_count % 100000 == 0) {
        fprintf(stderr, "harness: %llu steps, CS:IP=%04X:%04X\n",
                (unsigned long long)harness_step_count,
                regs16[REF_REG_CS], reg_ip);
        fflush(stderr);
    }

    if (harness_heartbeat_fp &&
        harness_heartbeat_every > 0 &&
        harness_step_count % harness_heartbeat_every == 0) {
        harness_emit_heartbeat();
    }

if (harness_step_count >= harness_step_limit) {
        fprintf(stderr, "harness: reached step limit %llu with no divergence. "
                        "Final CS:IP=%04X:%04X\n",
                (unsigned long long)harness_step_count,
                regs16[REF_REG_CS], reg_ip);
        uint32_t lin = ((uint32_t)regs16[REF_REG_CS] << 4) + reg_ip;
        fprintf(stderr, "harness: opcode bytes at CS:IP: ");
        for (int i = 0; i < 8; i++) fprintf(stderr, "%02X ", mem[lin + i]);
        fprintf(stderr, "\n");

        /* Diagnostic — ref side interrupt/vector state */
        fprintf(stderr, "harness: ref  int8_asap=%d IF=%d TF=%d seg_ovr_en=%d rep_ovr_en=%d\n",
                int8_asap, !!regs8[REF_FLAG_IF], !!regs8[REF_FLAG_TF],
                seg_override_en, rep_override_en);
        fprintf(stderr, "harness: ours int8_asap=%d IF=%d TF=%d seg_ovr_en=%d rep_ovr_en=%d\n",
                our_state->int8_asap,
                !!(our_state->flags & FLAG_IF),
                !!(our_state->flags & FLAG_TF),
                our_state->seg_override_en, our_state->rep_override_en);
        fprintf(stderr, "harness: vec 0x07 (int7)  mem[1C..1F]=%02X %02X %02X %02X (IP=%02X%02X CS=%02X%02X)\n",
                mem[0x1C], mem[0x1D], mem[0x1E], mem[0x1F],
                mem[0x1D], mem[0x1C], mem[0x1F], mem[0x1E]);
        fprintf(stderr, "harness: vec 0x0A (inta)  mem[28..2B]=%02X %02X %02X %02X (IP=%02X%02X CS=%02X%02X)\n",
                mem[0x28], mem[0x29], mem[0x2A], mem[0x2B],
                mem[0x29], mem[0x28], mem[0x2B], mem[0x2A]);

        fflush(stderr);
        exit(0);
    }
}

/* ================================================================
 * EMU-17: scratch-file disk isolation
 *
 * main() below wraps reference_main() (the reference's real main is
 * renamed via -Dmain=reference_main in HARNESS_REF_CFLAGS). Before the
 * reference opens any disks, we:
 *   1. copy the floppy (and HD, if provided) to two scratch files per
 *      image — one for the reference, one for us;
 *   2. rewrite argv so the reference opens its .ref.scratch files;
 *   3. stash the .hux.scratch paths in globals for harness_init to open
 *      on our side.
 *
 * The source image is never opened for writing by either emulator.
 * Scratch files are preserved after exit so divergent runs can be diff'd.
 * ================================================================ */

/* Reference's real main(), renamed by -Dmain=reference_main. */
int reference_main(int argc, char **argv);

/* Robust chunked copy. Returns 0 on success, -1 on error (with errno set and
 * an explanatory message already printed). */
static int
copy_file(const char *src, const char *dst)
{
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, "harness: cannot open source '%s': %s\n",
                src, strerror(errno));
        return -1;
    }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        fprintf(stderr, "harness: cannot create scratch '%s': %s\n",
                dst, strerror(errno));
        close(sfd);
        return -1;
    }
    uint8_t buf[64 * 1024];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "harness: write to '%s' failed: %s\n",
                        dst, strerror(errno));
                close(sfd); close(dfd);
                return -1;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "harness: read from '%s' failed: %s\n",
                src, strerror(errno));
        close(sfd); close(dfd);
        return -1;
    }
    if (close(dfd) != 0) {
        fprintf(stderr, "harness: close '%s' failed: %s\n",
                dst, strerror(errno));
        close(sfd);
        return -1;
    }
    close(sfd);
    return 0;
}

/* Build "<HARNESS_SCRATCH_DIR>/<basename(src)><suffix>" in `out`. Returns 0 on
 * success, -1 if the result would overflow PATH_MAX. */
static int
build_scratch_path(char *out, size_t outsz, const char *src, const char *suffix)
{
    /* basename() may modify its argument on some platforms; work on a copy. */
    char tmp[PATH_MAX];
    size_t slen = strlen(src);
    if (slen >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, src, slen + 1);
    const char *base = basename(tmp);
    int n = snprintf(out, outsz, "%s/%s%s", HARNESS_SCRATCH_DIR, base, suffix);
    if (n < 0 || (size_t)n >= outsz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <bios> <floppy> [@hd]\n"
        "  Runs lockstep against the reference. Each emulator gets its own\n"
        "  scratch copy of the disk images in %s/ — the source images are\n"
        "  never written. Scratch files are preserved after exit.\n",
        prog, HARNESS_SCRATCH_DIR);
}

/* EMU-31: if HARNESS_SPLIT_OUTPUT=1, route reference fd 1 into one fifo and
 * our emulator's console_out drain into another, so both streams can be
 * observed side-by-side during interactive use. The fifos live under
 * HARNESS_SCRATCH_DIR and are opened O_RDWR so the writer never blocks on
 * a missing reader. SIGPIPE is ignored so a reader Ctrl-C doesn't kill the
 * harness; writes past a disconnected reader simply return EPIPE and the
 * byte is dropped. */
static void
maybe_setup_split_output(void)
{
    const char *env = getenv("HARNESS_SPLIT_OUTPUT");
    if (!env || strcmp(env, "1") != 0) return;
    harness_split_output = 1;

    const char *ref_path = HARNESS_SCRATCH_DIR "/ref.out";
    const char *hux_path = HARNESS_SCRATCH_DIR "/hux.out";

    if (mkfifo(ref_path, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "harness: mkfifo(%s) failed: %s\n",
                ref_path, strerror(errno));
        exit(1);
    }
    if (mkfifo(hux_path, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "harness: mkfifo(%s) failed: %s\n",
                hux_path, strerror(errno));
        exit(1);
    }

    ref_fifo_fd = open(ref_path, O_RDWR);
    if (ref_fifo_fd < 0) {
        fprintf(stderr, "harness: open(%s) failed: %s\n",
                ref_path, strerror(errno));
        exit(1);
    }
    hux_fifo_fd = open(hux_path, O_RDWR);
    if (hux_fifo_fd < 0) {
        fprintf(stderr, "harness: open(%s) failed: %s\n",
                hux_path, strerror(errno));
        exit(1);
    }

    /* Survive a reader disconnect: writes to a broken pipe return EPIPE
     * instead of killing the process. */
    signal(SIGPIPE, SIG_IGN);

    /* Redirect reference's fd 1 onto the ref fifo. Must happen before
     * reference_main() runs any PUTCHAR. */
    if (dup2(ref_fifo_fd, 1) < 0) {
        fprintf(stderr, "harness: dup2 failed: %s\n", strerror(errno));
        exit(1);
    }

    fprintf(stderr, "harness: split-output mode active.\n"
                    "         Open a terminal and: cat %s\n"
                    "         Open another and:    cat %s\n",
                    ref_path, hux_path);
    fflush(stderr);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 1;
    }

    const char *bios_path   = argv[1];
    const char *floppy_src  = argv[2];
    const char *hd_arg      = (argc == 4) ? argv[3] : NULL;
    int         hd_boot     = (hd_arg && hd_arg[0] == '@');
    const char *hd_src      = hd_arg ? (hd_boot ? hd_arg + 1 : hd_arg) : NULL;

    if (mkdir(HARNESS_SCRATCH_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "harness: cannot create scratch dir '%s': %s\n",
                HARNESS_SCRATCH_DIR, strerror(errno));
        return 1;
    }

    /* EMU-24: heartbeat log. Parse cadence override, truncate-open the log,
     * and register an atexit handler so buffered lines are flushed on every
     * exit path (step-limit exit, divergence exit, error exit). */
    const char *hb_env = getenv("HARNESS_HEARTBEAT_EVERY");
    if (hb_env) {
        char *end;
        unsigned long long v = strtoull(hb_env, &end, 0);
        if (*end == '\0')
            harness_heartbeat_every = (uint64_t)v;
    }
    if (harness_heartbeat_every > 0) {
        harness_heartbeat_fp = fopen(HARNESS_HEARTBEAT_PATH, "w");
        if (!harness_heartbeat_fp) {
            fprintf(stderr, "harness: cannot open heartbeat log '%s': %s "
                            "(continuing without heartbeat)\n",
                    HARNESS_HEARTBEAT_PATH, strerror(errno));
        } else {
            atexit(harness_close_heartbeat);
        }
    }
    harness_last_heartbeat_us = harness_now_us();

    /* Floppy scratches */
    char floppy_ref[PATH_MAX], floppy_hux[PATH_MAX];
    if (build_scratch_path(floppy_ref, sizeof(floppy_ref),
                           floppy_src, ".ref.scratch") != 0 ||
        build_scratch_path(floppy_hux, sizeof(floppy_hux),
                           floppy_src, ".hux.scratch") != 0) {
        fprintf(stderr, "harness: floppy scratch path too long\n");
        return 1;
    }
    if (copy_file(floppy_src, floppy_ref) != 0) return 1;
    if (copy_file(floppy_src, floppy_hux) != 0) return 1;

    size_t n = strlen(floppy_hux);
    if (n >= sizeof(hux_floppy_path)) {
        fprintf(stderr, "harness: hux floppy path too long\n");
        return 1;
    }
    memcpy(hux_floppy_path, floppy_hux, n + 1);

    /* Optional HD scratches. We preserve the @-boot prefix in the arg
     * passed to the reference so it still sets DL=0x80 correctly. */
    char hd_ref[PATH_MAX] = "", hd_hux[PATH_MAX] = "";
    char hd_ref_arg[PATH_MAX + 1] = "";
    if (hd_src) {
        if (build_scratch_path(hd_ref, sizeof(hd_ref),
                               hd_src, ".ref.scratch") != 0 ||
            build_scratch_path(hd_hux, sizeof(hd_hux),
                               hd_src, ".hux.scratch") != 0) {
            fprintf(stderr, "harness: HD scratch path too long\n");
            return 1;
        }
        if (copy_file(hd_src, hd_ref) != 0) return 1;
        if (copy_file(hd_src, hd_hux) != 0) return 1;

        size_t hn = strlen(hd_hux);
        if (hn >= sizeof(hux_hd_path)) {
            fprintf(stderr, "harness: hux HD path too long\n");
            return 1;
        }
        memcpy(hux_hd_path, hd_hux, hn + 1);

        int k = snprintf(hd_ref_arg, sizeof(hd_ref_arg), "%s%s",
                         hd_boot ? "@" : "", hd_ref);
        if (k < 0 || (size_t)k >= sizeof(hd_ref_arg)) {
            fprintf(stderr, "harness: HD ref arg too long\n");
            return 1;
        }
    }

    fprintf(stderr, "harness: scratch files (preserved after exit):\n");
    fprintf(stderr, "  ref floppy: %s\n", floppy_ref);
    fprintf(stderr, "  hux floppy: %s\n", floppy_hux);
    if (hd_src) {
        fprintf(stderr, "  ref HD    : %s\n", hd_ref);
        fprintf(stderr, "  hux HD    : %s\n", hd_hux);
    }
    fflush(stderr);

    /* Build argv for reference_main: program, bios, floppy_ref, [hd_ref]. */
    char *ref_argv[5];
    int ref_argc = 0;
    ref_argv[ref_argc++] = argv[0];
    ref_argv[ref_argc++] = (char *)bios_path;
    ref_argv[ref_argc++] = floppy_ref;
    if (hd_src)
        ref_argv[ref_argc++] = hd_ref_arg;
    ref_argv[ref_argc] = NULL;

    /* EMU-31: optionally split reference fd 1 and our console_out drain into
     * two named pipes, for parallel observation. Must happen after the
     * scratch-file banner (goes to stderr, unaffected by the fd 1 redirect)
     * and before reference_main() runs any PUTCHAR. */
    maybe_setup_split_output();

    return reference_main(ref_argc, ref_argv);
}
