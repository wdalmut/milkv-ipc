#!/bin/sh
# setup-sdk.sh - materializza un SDK Sophgo deterministico:
#
#     SDK = SDK_REF pinnato  +  sdk-patches/*.patch  +  symlink di rtos/
#
# Tutto cio' che ci mettiamo di nostro e' versionato in QUESTO repo. L'SDK e'
# usa-e-getta: viene resettato hard a ogni run, quindi non accumula stato e
# non serve committarci dentro nulla. Nessun passo manuale.
#
#   ./scripts/setup-sdk.sh [percorso-sdk]
#
# Variabili: SDK_DIR, SKIP_PATCHES=1 (per il flusso di refresh-patches.sh)
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/sdk.lock"

SDK_DIR=${1:-${SDK_DIR:-$HOME/git/duo-buildroot-sdk}}

# ---------------------------------------------------------------- clone/fetch
if [ ! -d "$SDK_DIR/.git" ]; then
	echo ">> clono l'SDK in $SDK_DIR (diversi GB, mettiti comodo)"
	git clone --recurse-submodules "$SDK_URL" "$SDK_DIR"
fi

# ------------------------------------------------------- reset alla revisione
# Distruttivo per definizione: qualsiasi modifica locale all'SDK e' persa.
# E' il punto: se una modifica ti serve, deve stare in sdk-patches/.
if ! git -C "$SDK_DIR" diff --quiet || ! git -C "$SDK_DIR" diff --cached --quiet; then
	echo "!! l'SDK ha modifiche locali non committate."
	echo "   Se ti servono, catturale prima con:  ./scripts/refresh-patches.sh"
	printf "   Procedo a scartarle? [y/N] "
	read -r ans
	[ "$ans" = "y" ] || [ "$ans" = "Y" ] || { echo "annullato"; exit 1; }
fi

echo ">> allineo l'SDK a $SDK_REF"
git -C "$SDK_DIR" fetch --all --tags --quiet
git -C "$SDK_DIR" checkout --quiet "$SDK_REF"
git -C "$SDK_DIR" reset --hard --quiet "$SDK_REF"
git -C "$SDK_DIR" clean -fdq
git -C "$SDK_DIR" submodule update --init --recursive --quiet

# ----------------------------------------------------------- patch series
if [ "$SKIP_PATCHES" != "1" ]; then
	echo ">> applico sdk-patches/"
	FAILED=""
	for p in "$HERE"/sdk-patches/*.patch; do
		[ -e "$p" ] || continue
		name=$(basename "$p")
		# --3way sopravvive a piccoli spostamenti di contesto upstream
		if git -C "$SDK_DIR" apply --3way --whitespace=nowarn "$p" 2>/dev/null; then
			echo "   ok   $name"
		else
			echo "   FAIL $name"
			FAILED="$FAILED $name"
		fi
	done

	if [ -n "$FAILED" ]; then
		cat <<EOF

!! patch fallite:$FAILED

   Quasi sempre significa che l'upstream ha spostato il contesto (o che
   SDK_REF non e' quello contro cui le patch sono state generate).
   Applicale a mano nell'SDK e poi rigenerale con:

       ./scripts/refresh-patches.sh $SDK_DIR

   Meglio fallire qui, rumorosamente, che scoprirlo a runtime sulla board.
EOF
		exit 1
	fi
fi

# ------------------------------------------------------------ symlink sorgenti
# I .c/.h non stanno nelle patch: restano qui e sono symlinkati, cosi' li
# modifichi e ricompili senza rigenerare nulla.
"$HERE/scripts/install-rtos.sh" "$SDK_DIR"

echo "$SDK_DIR" > "$HERE/.sdk-path"

# ------------------------------------------------------- package in menuconfig
# Attiva il pacchetto nel defconfig Buildroot della board. Non e' un `cat >>`:
# BR2_ROOTFS_OVERLAY e' gia' impostata e va appesa, non sostituita.
"$HERE/scripts/enable-package.sh" "$SDK_DIR"

cat <<EOF

>> SDK pronto e deterministico. Per compilare:

     export BR2_EXTERNAL=$HERE
     cd $SDK_DIR && ./build.sh $SDK_TARGET

   Se build.sh non propaga BR2_EXTERNAL (dipende dalla revisione):
     make -C buildroot BR2_EXTERNAL=$HERE ${SDK_TARGET}_defconfig

EOF
