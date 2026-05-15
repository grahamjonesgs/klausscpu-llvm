#!/bin/bash
# get-freertos.sh — clone the FreeRTOS kernel into freertos/FreeRTOS-Kernel/.
# Run once from this directory before building.

set -euo pipefail
cd "$(dirname "$0")"

DEST="FreeRTOS-Kernel"
TAG="V11.1.0"   # tested with this release; update as needed

if [ -d "$DEST" ]; then
    echo "==> $DEST already present — skipping clone"
    echo "    To update: rm -rf $DEST && bash get-freertos.sh"
    exit 0
fi

echo "==> Cloning FreeRTOS-Kernel $TAG into $DEST ..."
git clone --depth 1 --branch "$TAG" \
    https://github.com/FreeRTOS/FreeRTOS-Kernel.git "$DEST"

echo ""
echo "==> Done.  Now run:  make demo.elf"
