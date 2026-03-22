#ifndef EMU86_SNAPSHOT_H
#define EMU86_SNAPSHOT_H

#include <stdint.h>
#include "state.h"

#define EMU86_SNAPSHOT_MAGIC   0x53363845  /* "E86S" in little-endian */
#define EMU86_SNAPSHOT_VERSION 1

/*
 * Snapshot header layout:
 *   [4 bytes] magic (EMU86_SNAPSHOT_MAGIC, little-endian)
 *   [4 bytes] version (little-endian)
 *   [4 bytes] data size (bytes after header, before checksum)
 *   [... field-by-field state data ...]
 *   [4 bytes] CRC32 checksum of everything above
 */

/* Returns the exact number of bytes needed for a snapshot. */
uint32_t emu86_snapshot_size(void);

/*
 * Serialise state to buf. Returns bytes written, or 0 on error
 * (buffer too small).
 */
uint32_t emu86_snapshot_save(const Emu86State *state, uint8_t *buf, uint32_t buf_size);

/*
 * Restore state from buf. Returns 0 on success, or:
 *   -1 = bad magic
 *   -2 = bad version
 *   -3 = bad checksum
 *   -4 = bad size
 */
int emu86_snapshot_restore(Emu86State *state, const uint8_t *buf, uint32_t buf_size);

#endif /* EMU86_SNAPSHOT_H */
