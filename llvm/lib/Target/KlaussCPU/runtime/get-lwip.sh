#!/bin/bash
# get-lwip.sh — clone the lwIP TCP/IP stack into runtime/lwip/.
# Run once from this directory before building the network demos
# (test_eth, eth_test, lwip_demo, ping_demo, tcp_echo, http_server, net_client)
# or the FreeRTOS net_demo / console_demo.
#
# Our lwIP port (sys_arch, ethernetif, cc.h, lwipopts.h) lives in lwip_port/
# and freertos/, not here — this clone provides only the upstream core.

set -euo pipefail
cd "$(dirname "$0")"

DEST="lwip"
TAG="STABLE-2_2_0_RELEASE"   # tested with lwIP 2.2.0; update as needed

if [ -d "$DEST" ]; then
    echo "==> $DEST already present — skipping clone"
    echo "    To update: rm -rf $DEST && bash get-lwip.sh"
    exit 0
fi

echo "==> Cloning lwIP $TAG into $DEST ..."
git clone --depth 1 --branch "$TAG" \
    https://github.com/lwip-tcpip/lwip.git "$DEST"

echo ""
echo "==> Done.  Now run e.g.:  make ping_demo.elf"
