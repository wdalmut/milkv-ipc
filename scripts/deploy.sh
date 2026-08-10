#!/bin/sh
# Ricompila solo il pacchetto duos-ipc e lo copia sulla board, senza rifare
# l'immagine intera. Ciclo di iterazione da ~10 secondi invece che ~10 minuti.
#
#   ./scripts/deploy.sh [ip-board]
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK_DIR=$(cat "$HERE/.sdk-path" 2>/dev/null || echo "$HOME/src/duo-buildroot-sdk")
BOARD_IP=${1:-192.168.42.1}
. "$HERE/sdk.lock"

BR_OUT="$SDK_DIR/buildroot/output/$SDK_TARGET"
[ -d "$BR_OUT" ] || BR_OUT="$SDK_DIR/buildroot/output"

echo ">> rebuild del solo pacchetto"
make -C "$BR_OUT" BR2_EXTERNAL="$HERE" duos-ipc-rebuild

BIN="$BR_OUT/build/duos-ipc-1.0/reader"
[ -f "$BIN" ] || { echo "!! binario non trovato: $BIN" >&2; exit 1; }

echo ">> copio su root@$BOARD_IP"
scp "$BIN" "root@$BOARD_IP:/usr/bin/ipc-reader"
scp "$HERE/board/duos/rootfs-overlay/root/selftest.sh" "root@$BOARD_IP:/root/"

echo ">> fatto. Sulla board:  ipc-reader -n 10   oppure   /root/selftest.sh"
