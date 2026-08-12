#!/bin/sh
# install-rtos.sh - symlinka i sorgenti del task FreeRTOS dentro l'SDK.
#
#   ./scripts/install-rtos.sh <percorso-sdk>
#   REPO_PATH=/data ./scripts/install-rtos.sh /sdk     # se compili in container
#
# Solo symlink, nessuna copia: modifichi rtos/sensor_task.c qui e ricompili
# nell'SDK senza altri passaggi. Le modifiche ai file DELL'SDK (CMakeLists,
# main.c, DTS) non stanno qui: stanno in sdk-patches/.
#
# ATTENZIONE, i symlink sono ASSOLUTI. Se compili dentro un container, devono
# puntare al path del repo COME LO VEDE il container, non a quello dell'host: la
# stessa directory ha due nomi diversi ai due lati e non esiste un path relativo
# che vada bene per entrambi. Da qui REPO_PATH.
#
# Il sintomo, se sbagli, e' un CMake che si ferma su
#   Cannot find source file: .../task/ipc/sensor_task.c
# perche' il symlink c'e' ma non risolve.
#
# Idempotente.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK_DIR=${1:-$(cat "$HERE/.sdk-path" 2>/dev/null || echo "")}

# Il path di questo repo come lo vede chi compila.
#
# Si ricorda in .repo-path, cosi' un ./scripts/setup-sdk.sh senza variabili non
# riporta i symlink all'host annullando la scelta in silenzio. E' il modo in cui
# ci siamo cascati la prima volta.
REPO_PATH=${REPO_PATH:-$(cat "$HERE/.repo-path" 2>/dev/null || echo "$HERE")}

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
ln -sf "$REPO_PATH/rtos/sensor_task.c"      "$DEST/sensor_task.c"
ln -sf "$REPO_PATH/src/shared/shared_msg.h" "$DEST/shared_msg.h"

echo ">> collegato $REPO_PATH/rtos -> $DEST"

if [ "$REPO_PATH" = "$HERE" ]; then
	rm -f "$HERE/.repo-path"
	# Nessun override: i symlink devono risolvere anche da qui, e se non lo
	# fanno e' un errore vero, non un artefatto di container.
	if [ ! -r "$DEST/sensor_task.c" ] || [ ! -r "$DEST/shared_msg.h" ]; then
		echo "!! i symlink non risolvono: sorgenti mancanti in $REPO_PATH" >&2
		exit 1
	fi
else
	echo "$REPO_PATH" > "$HERE/.repo-path"
	echo "   ricordato in .repo-path: i prossimi run useranno questo path"
	echo "   anche senza la variabile. Per tornare all'host: REPO_PATH=$HERE"
fi
