# Cross-compilazione del consumer di esempio in Go.
#
#   make                              # per la board (riscv64)
#   make GOARCH=arm64                 # per la variante glibc-arm64 del DuoS
#   make deploy BOARD_IP=192.168.1.50
#
# CGO_ENABLED=0 e' la ragione per cui tutto questo funziona senza toolchain
# incrociata: il binario esce statico e non linka libc, quindi gira sul rootfs
# musl della board senza dipendenze. Con cgo attivo servirebbe un
# riscv64-unknown-linux-musl-gcc e non basterebbe piu' un `go build`.

BIN     ?= ipc-reader-go
GOARCH  ?= riscv64
BOARD_IP ?= 192.168.42.1

# -s -w: via tabella dei simboli e DWARF. Su un binario Go non e' cosmetica,
# sono circa 800 KB su 2,2 - e su una board non ci si debugga comunque.
LDFLAGS ?= -s -w

.PHONY: all vet native deploy clean

all: $(BIN)

$(BIN): main.go go.mod
	GOOS=linux GOARCH=$(GOARCH) CGO_ENABLED=0 \
		go build -ldflags="$(LDFLAGS)" -o $@ .
	@# Conferma che sia davvero per la board e davvero statico: un GOARCH
	@# dimenticato produce un binario x86 che sulla board da' "not found",
	@# messaggio che manda a cercare il file sbagliato.
	@file $@ 2>/dev/null || true

vet:
	go vet ./...

# Per l'host: non trova il device e muore sull'open, ma verifica i tipi e
# permette di provare il percorso d'errore senza board.
native:
	CGO_ENABLED=0 go build -o $(BIN)-native .

deploy: $(BIN)
	scp $(BIN) root@$(BOARD_IP):/root/
	@echo ">> sulla board:  /root/$(BIN) -n 10"
	@echo "   e senza root: su -s /bin/sh -c \"/root/$(BIN) -n 5\" nobody"

clean:
	rm -f $(BIN) $(BIN)-native
