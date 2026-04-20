#!/bin/bash
# Semi-automated FreeDOS boot test
# Sends "dir" + "quitemu" via stdin, checks for expected output

set -e

cd "$(dirname "$0")/../.."

if [ ! -f emu86 ]; then
    echo "Building emu86..."
    make emu86
fi

if [ ! -f test/images/freedos.img ]; then
    echo "SKIP: test/images/freedos.img not found"
    exit 0
fi

echo "Booting FreeDOS (10 second timeout)..."
timeout 10 ./emu86 reference/bios test/images/freedos.img <<'EOF' 2>/dev/null | tee /tmp/emu86_boot_output.txt || true
dir
quitemu
EOF

if grep -qi "COMMAND\|FreeDOS\|A:\\\\>" /tmp/emu86_boot_output.txt; then
    echo "PASS: FreeDOS boot output detected"
    exit 0
else
    echo "FAIL: Expected FreeDOS boot output not found"
    echo "--- Output ---"
    head -50 /tmp/emu86_boot_output.txt
    echo "--- End ---"
    exit 1
fi
