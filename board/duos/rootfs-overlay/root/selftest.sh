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

echo "== 1. il canale risponde?"
if ! ipc-reader -1; then
	echo "FAIL: nessun campione. Il task FreeRTOS gira? Il modulo e' caricato?"
	echo "      lsmod | grep duos ; ipc-reader --devmem -1"
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

echo "== 3. il device c'e' ed e' leggibile da tutti"
if [ ! -c /dev/duos-ipc ]; then
	echo "FAIL: /dev/duos-ipc assente."
	echo "      Modulo non caricato, o nodo duos_ipc mancante nel device tree:"
	echo "      il driver si lega a compatible = corley,duos-ipc."
	exit 1
fi
ls -l /dev/duos-ipc

# Il senso del redesign: i dati escono dal device, non da /dev/mem. Se questo
# passa, il canale non richiede piu' root ne' CONFIG_STRICT_DEVMEM disattivato.
echo "== 4. si legge SENZA root"
if su -s /bin/sh -c "ipc-reader -1" nobody > "$TMP" 2>&1; then
	echo "   ok, come utente nobody"
else
	echo "FAIL: da utente non privilegiato non funziona."
	cat "$TMP"
	echo "      Permessi del device? Il driver lo crea con mode 0444."
	exit 1
fi

echo "== 5. raccolgo $N campioni"
ipc-reader -n "$N" > "$TMP" || true
GOT=$(grep -c '^seq=' "$TMP" || true)
echo "   ricevuti: $GOT / $N"
[ "$GOT" -ge "$N" ] || { echo "FAIL: producer fermo o troppo lento"; exit 1; }

echo "== 6. nessun campione perso"
if grep -q 'PERSI' "$TMP"; then
	echo "   WARN: campioni sovrascritti prima di essere letti"
	echo "         doorbell accorpate: confronta con /sys/class/misc/duos-ipc/bell"
fi

echo "== 7. seq monotona"
awk '
  /^seq=/ {
    split($1, a, "="); s = a[2] + 0
    if (NR > 1 && s <= prev) { print "FAIL: seq non monotona:", prev, "->", s; bad = 1 }
    prev = s
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo "== 8. timestamp crescenti (niente letture stantie da cache)"
awk '
  { for (i = 1; i <= NF; i++) if ($i ~ /^ts=/) { split($i, a, "="); t = a[2] + 0 } }
  /^seq=/ {
    if (n++ && t <= prev_t) { print "FAIL: ts non crescente -> sospetta cache non invalidata"; bad = 1 }
    prev_t = t
  }
  END { exit bad ? 1 : 0 }
' "$TMP" || exit 1

echo "== 9. dmesg: nessun doorbell orfano DOPO il caricamento"
# "error ip=6 , cmd=64" e' il ramo dell'ISR di cvi_rtos_cmdqu che non trova ne'
# un waiter ne' un handler registrato. Al boot ne compare sempre una manciata:
# l'RTOS suona gia' mentre il driver del mailbox ha fatto probe ma il nostro
# modulo non e' ancora caricato. Quelle sono attese. Quelle che contano sono le
# righe DOPO il messaggio del modulo: li' vorrebbe dire che l'handler e' stato
# scalzato, o che il modulo e' stato scaricato.
# Attenzione: grep -n da' il NUMERO DI RIGA in dmesg, che serve a ordinare gli
# eventi ma non e' un conteggio. Il conteggio va chiesto a parte, o si finisce a
# leggere "318" come se fossero 318 doorbell perse.
LAST_ERR=$(dmesg | grep -n 'error ip=6' | tail -1 | cut -d: -f1)
LAST_MOD=$(dmesg | grep -n 'duos-ipc.*pronto' | tail -1 | cut -d: -f1)
N_ERR=$(dmesg | grep -c 'error ip=6' || true)

if [ -z "$LAST_MOD" ]; then
	echo "   WARN: nessun messaggio di caricamento del modulo in dmesg"
elif [ -n "$LAST_ERR" ] && [ "$LAST_ERR" -gt "$LAST_MOD" ]; then
	echo "FAIL: doorbell orfane dopo il caricamento del modulo."
	echo "      Handler scalzato da una RTOS_CMDQU_REQUEST su IP_SYSTEM?"
	dmesg | tail -5
	exit 1
else
	echo "   ok ($N_ERR doorbell orfane, tutte precedenti al caricamento)"
fi

echo
echo "PASS"
