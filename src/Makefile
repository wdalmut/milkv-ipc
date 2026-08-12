# Compilato da Buildroot (che passa CC e CFLAGS) oppure a mano con
#   make CC=riscv64-unknown-linux-musl-gcc
#
# Il modulo kernel NON si costruisce da qui: sta in kmod/ e vuole il Makefile del
# kernel dell'SDK. Ci pensa il package Buildroot.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I.
LDFLAGS ?=

BINS = reader ipc_cmd

all: $(BINS)

reader: reader.c shared/shared_msg.h
	$(CC) $(CFLAGS) -o $@ reader.c $(LDFLAGS)

# Non include shared_msg.h: manda comandi da 8 byte sul mailbox e non tocca la
# finestra condivisa.
ipc_cmd: ipc_cmd.c
	$(CC) $(CFLAGS) -o $@ ipc_cmd.c $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean
