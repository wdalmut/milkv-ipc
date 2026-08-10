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

echo "== 1. magic presente?"
if ! ipc-reader -1; then
	echo "FAIL: magic assente. Il task FreeRTOS gira? L'indirizzo combacia?"
	exit 1
fi

echo "== 2. raccolgo $N campioni"
ipc-reader -n "$N" -i 5 > "$TMP" || true
GOT=$(grep -c '^seq=' "$TMP" || true)
echo "   ricevuti: $GOT / $N"
[ "$GOT" -ge "$N" ] || { echo "FAIL: producer fermo o troppo lento"; exit 1; }

echo "== 3. seq monotona e senza buchi"
if grep -q 'MISSED' "$TMP"; then
	echo "WARN: campioni saltati - polling troppo lento o producer troppo veloce"
fi

awk '
  /^seq=/ {
    split($1, a, "="); s = a[2] + 0
    if (NR > 1 && s <= prev) { print "FAIL: seq non monotona:", prev, "->", s; bad = 1 }
    prev = s
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo "== 4. timestamp crescenti (niente letture stantie da cache)"
awk '
  { for (i = 1; i <= NF; i++) if ($i ~ /^ts=/) { split($i, a, "="); t = a[2] + 0 } }
  /^seq=/ {
    if (n++ && t <= prev_t) { print "FAIL: ts non crescente -> sospetta cache non invalidata"; bad = 1 }
    prev_t = t
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo
echo "PASS"
