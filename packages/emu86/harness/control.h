/*
 * harness/control.h — transport-agnostic control plane (EMU-34)
 *
 * Defines the pure-function command dispatcher used by the harness
 * control plane. The same code runs under the Linux UDP wrapper and
 * (eventually) the WASM postMessage wrapper: neither path is visible
 * here — it's all NUL-terminated strings in caller-provided buffers.
 *
 * Linux UDP transport lives in harness/harness.c; WASM transport will
 * be a separate tiny adapter. Both call harness_control_handle().
 */
#ifndef EMU86_HARNESS_CONTROL_H
#define EMU86_HARNESS_CONTROL_H

#include <stddef.h>

typedef enum { VT_BOOL, VT_U64 } ControlVarType;

typedef struct {
    const char     *name;
    ControlVarType  type;
    int             read_only;
    void           *ptr;   /* &harness_fast_compare, &harness_step_count, ... */
} ControlVar;

/*
 * Register the variable table used by `get`/`set`. The caller retains
 * ownership of the array; the dispatcher just stashes the pointer.
 * The table must be terminated by an entry with name==NULL.
 */
void harness_control_set_vars(const ControlVar *vars);

/*
 * Handle one command. `cmd` is a NUL-terminated input string; `response`
 * is a caller-provided buffer of size `response_size` that will receive
 * a NUL-terminated reply.
 *
 * No socket, fd, stdio, or allocation calls — identical on Linux and WASM.
 */
void harness_control_handle(const char *cmd, char *response, size_t response_size);

#endif /* EMU86_HARNESS_CONTROL_H */
