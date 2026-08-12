// ipc-reader-go - consumer del canale IPC in Go, come prova che il device e'
// usabile da qualunque linguaggio senza cgo, senza mmap e senza root.
//
// Si compila in locale e si copia sulla board:
//
//	GOOS=linux GOARCH=riscv64 CGO_ENABLED=0 go build -ldflags="-s -w" -o ipc-reader-go .
//	scp ipc-reader-go root@<board>:/root/
//
// CGO_ENABLED=0 e' la parte che conta: il binario e' statico e non linka libc,
// quindi non serve una toolchain musl per riscv64 e gira sul rootfs della board
// senza dipendenze.
package main

import (
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"syscall"
)

const (
	devPath  = "/dev/duos-ipc"
	msgMagic = 0xC0FFEE01
	msgSize  = 32
)

// SensorMsg rispecchia sensor_msg_t di src/shared/shared_msg.h.
//
// Ordine e tipi devono combaciare byte per byte con quello che scrive il core
// C906: 32 byte, little-endian. Il campo senza nome e' il padding esplicito
// della struct C - binary.Read consuma quei byte e li scarta, che e' esattamente
// cio' che serve per restare allineati sul campione successivo.
type SensorMsg struct {
	Magic  uint32
	Seq    uint32
	TempMC int32
	VibRMS uint32
	TsUs   uint64
	Drops  uint32
	_      uint32
}

func main() {
	n := flag.Int("n", 0, "esce dopo N campioni (0 = infinito)")
	flag.Parse()

	// Se questa non torna, la struct Go e il device non parlano la stessa
	// lingua: il primo campione sembrerebbe plausibile e tutti i successivi
	// sarebbero disallineati di qualche byte. Meglio fermarsi subito.
	if size := binary.Size(SensorMsg{}); size != msgSize {
		fmt.Fprintf(os.Stderr, "struct da %d byte, il device ne manda %d\n",
			size, msgSize)
		os.Exit(1)
	}

	f, err := os.Open(devPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		fmt.Fprintln(os.Stderr, "il modulo e' caricato? lsmod | grep duos")
		os.Exit(1)
	}
	defer f.Close()

	var last uint32
	first := true

	for i := 0; *n == 0 || i < *n; i++ {
		var m SensorMsg

		// Blocca finche' il C906 non pubblica: nessun polling, nessuna
		// attesa attiva. Il driver ha gia' verificato il seqlock, quindi
		// il campione che arriva qui non e' strappato.
		if err := binary.Read(f, binary.LittleEndian, &m); err != nil {
			switch {
			case errors.Is(err, io.EOF):
				return
			case errors.Is(err, syscall.ENODATA):
				fmt.Fprintln(os.Stderr,
					"magic assente: il task FreeRTOS non ha ancora pubblicato")
			case errors.Is(err, syscall.EAGAIN):
				fmt.Fprintln(os.Stderr,
					"snapshot sempre strappato: producer troppo veloce?")
			default:
				fmt.Fprintln(os.Stderr, "read:", err)
			}
			os.Exit(1)
		}

		if m.Magic != msgMagic {
			// Il driver filtra gia' questo caso, quindi se ci arriviamo il
			// disallineamento e' nostro, non suo.
			fmt.Fprintf(os.Stderr, "magic inatteso 0x%08x: struct sbagliata?\n",
				m.Magic)
			os.Exit(1)
		}

		lost := uint32(0)
		if !first {
			lost = m.Seq - last - 1
		}
		first = false
		last = m.Seq

		fmt.Printf("seq=%-8d T=%7.3f C  vib=%-6d ts=%d us  drops=%d",
			m.Seq, float64(m.TempMC)/1000.0, m.VibRMS, m.TsUs, m.Drops)
		if lost != 0 {
			fmt.Printf("  [PERSI %d]", lost)
		}
		fmt.Println()
	}
}
