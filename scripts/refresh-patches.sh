#!/bin/sh
# refresh-patches.sh - cattura le modifiche fatte a mano nell'SDK e le
# riversa in sdk-patches/, cosi' tornano versionate qui.
#
#   ./scripts/refresh-patches.sh [percorso-sdk]
#
# Flusso tipico quando una release Sophgo rompe le patch:
#
#   1. SKIP_PATCHES=1 ./scripts/setup-sdk.sh     # SDK pulito alla nuova ref
#   2. ...applichi le 3 modifiche a mano nell'SDK...
#   3. ./scripts/refresh-patches.sh              # le ricattura qui
#   4. aggiorni SDK_REF in sdk.lock, commit
#
# Da qui in poi setup-sdk.sh e' di nuovo a zero passi manuali.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
SDK_DIR=${1:-$(cat "$HERE/.sdk-path" 2>/dev/null || echo "$HOME/src/duo-buildroot-sdk")}

[ -d "$SDK_DIR/.git" ] || { echo "!! SDK non trovato in $SDK_DIR" >&2; exit 1; }

OUT="$HERE/sdk-patches"
mkdir -p "$OUT"

if git -C "$SDK_DIR" diff --quiet HEAD; then
	echo "!! nessuna modifica nell'SDK da catturare." >&2
	echo "   (i symlink dei sorgenti non contano: quelli non vanno in patch)" >&2
	exit 1
fi

echo ">> file modificati nell'SDK:"
git -C "$SDK_DIR" diff --name-only HEAD | sed 's/^/   /'

# Un singolo diff cumulativo: piu' onesto di tre patch scollegate quando non
# sappiamo quali file l'upstream ha spostato. Se preferisci la serie granulare,
# committa nell'SDK in 3 commit separati e usa `git format-patch`.
STAMP=$(date +%Y%m%d)
DEST="$OUT/9999-local-changes-$STAMP.patch"

{
	echo "From: duos-ipc <noreply@corley.it>"
	echo "Subject: [PATCH] modifiche locali all'SDK catturate il $STAMP"
	echo ""
	echo "Rigenerata da scripts/refresh-patches.sh contro:"
	echo "  $(git -C "$SDK_DIR" rev-parse --short HEAD) ($(git -C "$SDK_DIR" describe --tags --always 2>/dev/null || echo '-'))"
	echo ""
	echo "Rivedi il contenuto, poi spezzala nelle 0001/0002/0003 se ha senso"
	echo "e rimuovi le vecchie patch superate."
	echo ""
	git -C "$SDK_DIR" diff HEAD
} > "$DEST"

echo
echo ">> scritta $DEST"
echo "   Rileggila, riorganizza sdk-patches/, aggiorna SDK_REF in sdk.lock, committa."
