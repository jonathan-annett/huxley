/*
 * harness/control.c — transport-agnostic control plane (EMU-34)
 *
 * The command dispatcher is deliberately free of socket/fd/stdio calls.
 * It operates on NUL-terminated strings in caller-provided buffers so
 * the same code runs under the Linux UDP wrapper and the future WASM
 * postMessage wrapper without modification.
 */

#include "control.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Variable table (registered by the transport layer at startup)
 * ================================================================ */

static const ControlVar *registered_vars;

void harness_control_set_vars(const ControlVar *vars)
{
    registered_vars = vars;
}

static int lookup_var(const char *name, const ControlVar **out)
{
    if (!registered_vars) return 0;
    for (const ControlVar *v = registered_vars; v->name; v++) {
        if (strcmp(v->name, name) == 0) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static int var_get_str(const ControlVar *v, char *out, size_t outsz)
{
    switch (v->type) {
    case VT_BOOL: {
        int val = *(int *)v->ptr;
        return snprintf(out, outsz, "%d", val ? 1 : 0);
    }
    case VT_U64: {
        uint64_t val = *(uint64_t *)v->ptr;
        return snprintf(out, outsz, "%llu", (unsigned long long)val);
    }
    }
    return snprintf(out, outsz, "?");
}

static int var_set_str(const ControlVar *v, const char *val,
                       char *errbuf, size_t errsz)
{
    switch (v->type) {
    case VT_BOOL:
        if (strcmp(val, "0") == 0) { *(int *)v->ptr = 0; return 1; }
        if (strcmp(val, "1") == 0) { *(int *)v->ptr = 1; return 1; }
        snprintf(errbuf, errsz, "expected 0 or 1");
        return 0;
    case VT_U64: {
        char *end;
        unsigned long long n = strtoull(val, &end, 10);
        if (end == val || *end != '\0') {
            snprintf(errbuf, errsz, "expected unsigned integer");
            return 0;
        }
        *(uint64_t *)v->ptr = (uint64_t)n;
        return 1;
    }
    }
    snprintf(errbuf, errsz, "unknown type");
    return 0;
}

/* ================================================================
 * Command handlers
 * ================================================================ */

static void cmd_ping(const char *args, char *resp, size_t rsz);
static void cmd_help(const char *args, char *resp, size_t rsz);
static void cmd_get(const char *args, char *resp, size_t rsz);
static void cmd_set(const char *args, char *resp, size_t rsz);

typedef struct {
    const char *verb;
    void (*handler)(const char *args, char *resp, size_t rsz);
    const char *help;
} Command;

static const Command control_commands[] = {
    { "ping", cmd_ping, "ping                  - sanity check, returns 'pong'" },
    { "help", cmd_help, "help                  - list commands and variables" },
    { "get",  cmd_get,  "get <name> | get all  - read a variable" },
    { "set",  cmd_set,  "set <name> <value>    - write a variable" },
    { NULL, NULL, NULL }
};

static void cmd_ping(const char *args, char *resp, size_t rsz)
{
    (void)args;
    snprintf(resp, rsz, "OK pong");
}

static void cmd_help(const char *args, char *resp, size_t rsz)
{
    (void)args;
    size_t off = 0;
    int n = snprintf(resp + off, rsz - off, "OK commands:\n");
    if (n < 0 || (size_t)n >= rsz - off) return;
    off += (size_t)n;

    for (const Command *c = control_commands; c->verb; c++) {
        n = snprintf(resp + off, rsz - off, "  %s\n", c->help);
        if (n < 0 || (size_t)n >= rsz - off) return;
        off += (size_t)n;
    }

    n = snprintf(resp + off, rsz - off, "variables:\n");
    if (n < 0 || (size_t)n >= rsz - off) return;
    off += (size_t)n;

    if (registered_vars) {
        for (const ControlVar *v = registered_vars; v->name; v++) {
            const char *ty = (v->type == VT_BOOL) ? "bool" : "u64";
            const char *rw = v->read_only ? "R " : "RW";
            n = snprintf(resp + off, rsz - off, "  %-18s %-4s %s\n",
                         v->name, ty, rw);
            if (n < 0 || (size_t)n >= rsz - off) return;
            off += (size_t)n;
        }
    }
}

static void cmd_get(const char *args, char *resp, size_t rsz)
{
    if (*args == '\0') {
        snprintf(resp, rsz, "ERR usage: get <name> | get all");
        return;
    }

    if (strcmp(args, "all") == 0) {
        size_t off = 0;
        int n = snprintf(resp + off, rsz - off, "OK\n");
        if (n < 0 || (size_t)n >= rsz - off) return;
        off += (size_t)n;
        if (!registered_vars) return;
        for (const ControlVar *v = registered_vars; v->name; v++) {
            char valbuf[64];
            var_get_str(v, valbuf, sizeof(valbuf));
            n = snprintf(resp + off, rsz - off, "%s=%s\n", v->name, valbuf);
            if (n < 0 || (size_t)n >= rsz - off) return;
            off += (size_t)n;
        }
        return;
    }

    const ControlVar *v;
    if (!lookup_var(args, &v)) {
        snprintf(resp, rsz, "ERR unknown variable '%s'", args);
        return;
    }

    char valbuf[64];
    var_get_str(v, valbuf, sizeof(valbuf));
    snprintf(resp, rsz, "OK %s", valbuf);
}

static void cmd_set(const char *args, char *resp, size_t rsz)
{
    if (*args == '\0') {
        snprintf(resp, rsz, "ERR usage: set <name> <value>");
        return;
    }

    /* Split name and value. */
    char name[128];
    const char *sp = strchr(args, ' ');
    if (!sp) {
        snprintf(resp, rsz, "ERR usage: set <name> <value>");
        return;
    }
    size_t nlen = (size_t)(sp - args);
    if (nlen == 0 || nlen >= sizeof(name)) {
        snprintf(resp, rsz, "ERR bad variable name");
        return;
    }
    memcpy(name, args, nlen);
    name[nlen] = '\0';
    const char *val = sp + 1;
    while (*val == ' ') val++;
    if (*val == '\0') {
        snprintf(resp, rsz, "ERR usage: set <name> <value>");
        return;
    }

    const ControlVar *v;
    if (!lookup_var(name, &v)) {
        snprintf(resp, rsz, "ERR unknown variable '%s'", name);
        return;
    }
    if (v->read_only) {
        snprintf(resp, rsz, "ERR %s is read-only", name);
        return;
    }

    char errbuf[64];
    if (!var_set_str(v, val, errbuf, sizeof(errbuf))) {
        snprintf(resp, rsz, "ERR %s", errbuf);
        return;
    }

    char valbuf[64];
    var_get_str(v, valbuf, sizeof(valbuf));
    snprintf(resp, rsz, "OK %s=%s", name, valbuf);
}

/* ================================================================
 * Core entry point
 * ================================================================ */

void harness_control_handle(const char *cmd, char *response, size_t response_size)
{
    if (response_size == 0) return;
    response[0] = '\0';

    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    if (*cmd == '\0' || *cmd == '\n' || *cmd == '\r') {
        snprintf(response, response_size, "ERR empty command");
        return;
    }

    /* Copy into local scratch so we can strip trailing newlines and
     * split verb/args without mutating the caller's buffer. */
    char buf[512];
    size_t len = 0;
    while (cmd[len] != '\0' && len + 1 < sizeof(buf)) {
        buf[len] = cmd[len];
        len++;
    }
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'
                    || buf[len - 1] == ' '  || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }
    if (len == 0) {
        snprintf(response, response_size, "ERR empty command");
        return;
    }

    char *verb = buf;
    char *args = strchr(buf, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') args++;
    } else {
        args = buf + len; /* points at the terminator — empty args */
    }

    for (const Command *c = control_commands; c->verb; c++) {
        if (strcmp(verb, c->verb) == 0) {
            c->handler(args, response, response_size);
            return;
        }
    }
    snprintf(response, response_size, "ERR unknown command '%s'", verb);
}
