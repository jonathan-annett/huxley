#!/usr/bin/env bash
# tools/vterm.sh — interactive cockpit pane for one side of the harness.
# Reads output from ref.out or hux.out in the background; forwards each
# keystroke typed in this terminal to the shared kbd.in fifo.
#
# Usage: bash tools/vterm.sh {ref|hux}
# Exit: Ctrl-D

set -euo pipefail

case "${1:-}" in
    ref|hux) ;;
    *) echo "usage: $0 {ref|hux}" >&2; exit 1 ;;
esac

SIDE="$1"
OUT_FIFO="/tmp/emu86-harness/${SIDE}.out"
KBD_FIFO="/tmp/emu86-harness/kbd.in"

OLD_STTY=$(stty -g)

# Background reader: persistent cat, survives harness restarts.
(
    while true; do
        if [[ -p "$OUT_FIFO" ]]; then
            cat "$OUT_FIFO"
        else
            sleep 0.5
        fi
    done
) &
READER_PID=$!

cleanup() {
    stty "$OLD_STTY" 2>/dev/null || true
    # Kill cat child first (while its parent still exists to be queried),
    # then the subshell itself.
    pkill -P "$READER_PID" 2>/dev/null || true
    kill  "$READER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Raw-ish mode: one byte at a time, no local echo, don't translate CR to LF
# (DOS expects the raw 0x0D on Enter).
stty -icanon -echo -icrnl min 1 time 0

while IFS= read -r -n 1 key; do
    # Ctrl-D (0x04) — exit cleanly. In raw mode it's just a byte, not EOF,
    # so we have to check for it explicitly.
    [[ "$key" == $'\x04' ]] && break

    # Forward to the shared keyboard pipe. Existence check so we fail quiet
    # when the harness isn't running; the keystroke is silently dropped.
    if [[ -p "$KBD_FIFO" ]]; then
        printf '%s' "$key" > "$KBD_FIFO"
    fi
done