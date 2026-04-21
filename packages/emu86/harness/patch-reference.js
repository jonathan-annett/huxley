#!/usr/bin/env node
/*
 * patch-reference.js — transforms reference/8086tiny.c for use in the
 * differential harness.
 *
 * Applies source-level substitutions that the preprocessor can't
 * reliably handle (glibc fortify-source defeats -Dread=harness_read
 * etc at the linker level). Reference source is never modified; this
 * script writes to stdout and the Makefile captures it to
 * harness/obj/reference_patched.c.
 *
 * Usage:  node patch-reference.js <input.c>  > output.c
 */

const fs = require('fs');

const input = fs.readFileSync(process.argv[2], 'utf8');

/*
 * Prototypes for the harness_* wrappers (defined in harness/overrides.c).
 * Without these, the patched call sites fall under C's implicit-int rule:
 * on x86_64 that truncates size_t arguments and mis-types pointer returns,
 * which causes segfaults at runtime. Prepending the prototypes after the
 * system headers would be ideal, but prepending to the whole TU is safe
 * because <sys/types.h>/<time.h>/<sys/timeb.h>/<unistd.h> are self-contained
 * and harmless to include early.
 */
const prologue = `/* injected by harness/patch-reference.js */
#include <sys/types.h>
#include <time.h>
#include <sys/timeb.h>
ssize_t harness_read(int fd, void *buf, size_t count);
time_t  harness_time(time_t *t);
int     harness_ftime(struct timeb *tp);
struct tm *harness_localtime(const time_t *t);
/* end injected */
`;

const patches = [
    {
        name: 'redirect read(...) to harness_read',
        from: /\bread\s*\(/g,
        to: 'harness_read(',
    },
    {
        name: 'redirect time(...) to harness_time',
        from: /\btime\s*\(/g,
        to: 'harness_time(',
    },
    {
        name: 'redirect ftime(...) to harness_ftime',
        from: /\bftime\s*\(/g,
        to: 'harness_ftime(',
    },
    {
        name: 'redirect localtime(...) to harness_localtime',
        from: /\blocaltime\s*\(/g,
        to: 'harness_localtime(',
    },
];

let output = input;
let totalReplaced = 0;
for (const p of patches) {
    const count = (output.match(p.from) || []).length;
    totalReplaced += count;
    output = output.replace(p.from, p.to);
    console.error(`patch-reference: ${count} occurrence${count === 1 ? '' : 's'} — ${p.name}`);
}
console.error(`patch-reference: total ${totalReplaced} substitutions`);

if (totalReplaced === 0) {
    console.error('patch-reference: WARNING — no substitutions applied; patches may be out of date');
    process.exit(1);
}

process.stdout.write(prologue + output);
