# Compilato da Buildroot (che passa CC e CFLAGS) oppure a mano con
#   make CC=riscv64-unknown-linux-musl-gcc
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I.
LDFLAGS ?=

BIN = reader

all: $(BIN)

$(BIN): reader.c shared/shared_msg.h
	$(CC) $(CFLAGS) -o $@ reader.c $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
