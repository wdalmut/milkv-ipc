#!/bin/sh
# Chiamato da Buildroot a immagine costruita (BR2_ROOTFS_POST_BUILD_SCRIPT).
# $1 = TARGET_DIR
set -e
TARGET_DIR=$1
chmod 0755 "$TARGET_DIR/root/selftest.sh" 2>/dev/null || true
