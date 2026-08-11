#!/bin/sh
# selftest.sh - verifica che il canale IPC sia vivo e coerente.
# Exit code 0 = ok. Usalo come regressione ogni volta che tocchi le barriere,
# la cache policy o l'indirizzo della finestra condivisa.
#
#   ./selftest.sh [numero-campioni]
set -e

N=${1:-20}
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

seq_now() {
	ipc-reader -1 | sed -n 's/.*seq=\([0-9]*\).*/\1/p'
}

echo "== 1. magic presente?"
if ! ipc-reader -1; then
	echo "FAIL: magic assente. Il task FreeRTOS gira? L'indirizzo combacia?"
	exit 1
fi

# Questo controllo e' il motivo per cui il selftest esiste. Un canale morto con
# magic valido e payload coerente supera qualunque lettura singola: e' esattamente
# come si e' presentato il bug del mailbox pieno, con seq congelata a 9 e tutto il
# resto in ordine. Solo due letture distanziate lo smascherano.
echo "== 2. seq avanza nel tempo (canale vivo, non congelato su dati validi)"
S1=$(seq_now)
sleep 1
S2=$(seq_now)
echo "   seq: $S1 -> $S2"
if [ "$S1" = "$S2" ]; then
	echo "FAIL: seq congelata. Producer fermo: RTOS in trap o task morto."
	echo "      Guarda la console seriale dell'RTOS."
	exit 1
fi

echo "== 3. modo di attesa"
if [ -c /dev/duos-ipc ]; then
	echo "   /dev/duos-ipc presente: attesa sul doorbell"
else
	echo "   WARN: /dev/duos-ipc assente, il reader ripieghera' sul polling."
	echo "         Immagine senza BR2_PACKAGE_DUOS_IPC_KMOD, o modulo non caricato."
fi

echo "== 4. raccolgo $N campioni"
ipc-reader -n "$N" > "$TMP" || true
GOT=$(grep -c '^seq=' "$TMP" || true)
echo "   ricevuti: $GOT / $N"
[ "$GOT" -ge "$N" ] || { echo "FAIL: producer fermo o troppo lento"; exit 1; }

echo "== 5. nessuna perdita"
# Due perdite distinte: [SEQ -n] = il producer ha sovrascritto campioni non
# letti; [BELL -n] = notifiche accorpate. La prima e' un problema di consumo,
# la seconda di notifica.
if grep -q 'SEQ -' "$TMP"; then
	echo "   WARN: campioni sovrascritti prima di essere letti"
fi
if grep -q 'BELL -' "$TMP"; then
	echo "   WARN: doorbell accorpate - coda mailbox satura?"
fi

echo "== 6. seq monotona"
awk '
  /^seq=/ {
    split($1, a, "="); s = a[2] + 0
    if (NR > 1 && s <= prev) { print "FAIL: seq non monotona:", prev, "->", s; bad = 1 }
    prev = s
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo "== 7. timestamp crescenti (niente letture stantie da cache)"
awk '
  { for (i = 1; i <= NF; i++) if ($i ~ /^ts=/) { split($i, a, "="); t = a[2] + 0 } }
  /^seq=/ {
    if (n++ && t <= prev_t) { print "FAIL: ts non crescente -> sospetta cache non invalidata"; bad = 1 }
    prev_t = t
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo "== 8. dmesg: nessun doorbell orfano"
# "error ip=6 , cmd=64" e' il ramo dell'ISR di cvi_rtos_cmdqu che non trova ne'
# un waiter ne' un handler registrato: e' la firma di doorbell accesa senza
# modulo. La sua assenza e' la prova che l'handler e' agganciato.
if dmesg | grep -q 'error ip=6'; then
	echo "FAIL: doorbell senza handler. duos_ipc_irq non caricato?"
	dmesg | grep 'error ip=6' | tail -3
	exit 1
fi

echo
echo "PASS"
