#!/bin/sh
# install-rtos.sh - symlinka i sorgenti del task FreeRTOS dentro l'SDK.
#
#   ./scripts/install-rtos.sh <percorso-sdk>
#
# Solo symlink, nessuna copia: modifichi rtos/sensor_task.c qui e ricompili
# nell'SDK senza altri passaggi. Le modifiche ai file DELL'SDK (CMakeLists,
# main.c, DTS) non stanno qui: stanno in sdk-patches/.
#
# Idempotente.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK_DIR=${1:-$(cat "$HERE/.sdk-path" 2>/dev/null || echo "")}

[ -n "$SDK_DIR" ] && [ -d "$SDK_DIR" ] || {
	echo "!! percorso SDK mancante o inesistente: '$SDK_DIR'" >&2
	exit 1
}

TASK_DIR="$SDK_DIR/freertos/cvitek/task"
[ -d "$TASK_DIR" ] || {
	echo "!! $TASK_DIR non trovato: layout dell'SDK cambiato?" >&2
	exit 1
}

DEST="$TASK_DIR/ipc"
mkdir -p "$DEST"
ln -sf "$HERE/rtos/sensor_task.c"      "$DEST/sensor_task.c"
ln -sf "$HERE/src/shared/shared_msg.h" "$DEST/shared_msg.h"

echo ">> collegato rtos/ -> $DEST"
