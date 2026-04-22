#!/usr/bin/env bash
# tools/harness-ctl.sh - send a control command to the harness and print
# the response. Requires HARNESS_CONTROL=1 on the harness side.
#
# Usage: bash tools/harness-ctl.sh <command> [args...]
# Examples:
#   bash tools/harness-ctl.sh ping
#   bash tools/harness-ctl.sh get step_count
#   bash tools/harness-ctl.sh set fast_compare 1
#   bash tools/harness-ctl.sh get all

set -euo pipefail

PORT="${HARNESS_CONTROL_PORT:-7071}"
HOST="${HARNESS_CONTROL_HOST:-127.0.0.1}"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 1
fi

printf '%s' "$*" | nc -u -w 1 "$HOST" "$PORT"
echo
