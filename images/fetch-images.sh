#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Fetching disk images ==="

# FreeDOS 1.44MB floppy image
# The Rugxulo image is recommended by 8086tiny docs
if [ ! -f freedos.img ]; then
    echo "Downloading FreeDOS floppy image..."
    echo "TODO: Add URL for FreeDOS floppy image"
    echo "Options:"
    echo "  - Rugxulo: https://sites.google.com/site/rugxulo/dskthree.zip"
    echo "  - FreeDOS: https://www.freedos.org/"
    echo ""
    echo "Download manually and place as: $SCRIPT_DIR/freedos.img"
else
    echo "FreeDOS image already present."
fi

# ELKs hard disk image
if [ ! -f elks.img ]; then
    echo "Downloading ELKs disk image..."
    echo "TODO: Add URL for ELKs disk image"
    echo "Options:"
    echo "  - ELKs releases: https://github.com/jbruchon/elks/releases"
    echo ""
    echo "Download manually and place as: $SCRIPT_DIR/elks.img"
else
    echo "ELKs image already present."
fi

# Copy to test locations
EMU_TEST="$SCRIPT_DIR/../packages/emu86/test/images"
if [ -f freedos.img ]; then
    cp freedos.img "$EMU_TEST/freedos.img"
    echo "Copied FreeDOS image to test/images/"
fi
if [ -f elks.img ]; then
    cp elks.img "$EMU_TEST/elks.img"
    echo "Copied ELKs image to test/images/"
fi

echo "=== Done ==="
