#!/bin/sh
# enable-package.sh - attiva il pacchetto duos-ipc nel defconfig Buildroot della
# board, mergiando configs/duos_ipc_defconfig.
#
#   ./scripts/enable-package.sh [percorso-sdk]
#
# Perche' serve uno script e non un `cat >>`: BR2_ROOTFS_OVERLAY e' GIA'
# impostata nel defconfig della board e punta all'overlay Milk-V, dentro cui
# l'SDK riversa tutto il rootfs cvitek. Una seconda assegnazione della stessa
# variabile vince sulla prima e ti svuota il rootfs. E' una lista separata da
# spazi: il nostro overlay va appeso, non sostituito.
#
# Idempotente: rilanciarlo non duplica nulla.
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
. "$HERE/sdk.lock"

SDK_DIR=${1:-$(cat "$HERE/.sdk-path" 2>/dev/null || echo "$HOME/git/duo-buildroot-sdk")}
DEFCONFIG="$SDK_DIR/buildroot/configs/${SDK_TARGET}_defconfig"

[ -f "$DEFCONFIG" ] || {
	echo "!! defconfig non trovato: $DEFCONFIG" >&2
	echo "   SDK_TARGET in sdk.lock e' '$SDK_TARGET': combacia con buildroot/configs/?" >&2
	exit 1
}

FRAG="$HERE/configs/duos_ipc_defconfig"
OVERLAY_APPEND=$(sed -n 's/^BR2_ROOTFS_OVERLAY_APPEND="\(.*\)"$/\1/p' "$FRAG")

echo ">> mergio duos_ipc_defconfig in $DEFCONFIG"

# 1. Le assegnazioni semplici: si sostituiscono se presenti, si appendono se no.
sed -n 's/^\(BR2_[A-Z0-9_]*\)=\(.*\)$/\1 \2/p' "$FRAG" | while read -r key val; do
	[ "$key" = "BR2_ROOTFS_OVERLAY_APPEND" ] && continue

	if grep -q "^$key=" "$DEFCONFIG"; then
		# Gia' presente: lo riscrivo, cosi' un cambio di valore attecchisce.
		sed -i "s|^$key=.*|$key=$val|" "$DEFCONFIG"
		echo "   agg. $key"
	else
		printf '%s=%s\n' "$key" "$val" >> "$DEFCONFIG"
		echo "   add  $key"
	fi
done

# 2. BR2_ROOTFS_OVERLAY: append, mai sostituzione.
if [ -n "$OVERLAY_APPEND" ]; then
	CUR=$(sed -n 's/^BR2_ROOTFS_OVERLAY="\(.*\)"$/\1/p' "$DEFCONFIG")

	case " $CUR " in
	*" $OVERLAY_APPEND "*)
		echo "   ok   BR2_ROOTFS_OVERLAY contiene gia' il nostro overlay"
		;;
	*)
		if [ -n "$CUR" ]; then
			sed -i "s|^BR2_ROOTFS_OVERLAY=.*|BR2_ROOTFS_OVERLAY=\"$CUR $OVERLAY_APPEND\"|" \
				"$DEFCONFIG"
			echo "   app. BR2_ROOTFS_OVERLAY (preservato '$CUR')"
		else
			printf 'BR2_ROOTFS_OVERLAY="%s"\n' "$OVERLAY_APPEND" >> "$DEFCONFIG"
			echo "   add  BR2_ROOTFS_OVERLAY"
		fi
		;;
	esac
fi

echo
echo ">> fatto. Ricorda che BR2_EXTERNAL deve essere il path di questo repo"
echo "   COME LO VEDE il build: dentro un container e' il path montato,"
echo "   non quello dell'host."
