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

# Usa shared_msg.h per host_cmd_t: il sottocomando `send` scrive nella finestra.
ipc_cmd: ipc_cmd.c shared/shared_msg.h
	$(CC) $(CFLAGS) -o $@ ipc_cmd.c $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean
