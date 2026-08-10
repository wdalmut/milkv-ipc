# duos-ipc

Banco di prova per il passaggio di una struct dal core **FreeRTOS** (C906 small)
a **Linux** sul Milk-V Duo S / SG2000, via memoria condivisa + doorbell sul
mailbox.

Il mailbox del SG2000 trasporta solo 8 byte (`cmdqu_t`): non ci passi una
struct. Il pattern e' **payload in memoria condivisa, notifica sul mailbox**.

## Creazione del container di lavoro

Per comodità utilizzo il container Docker messo a disposizione dal Milk-V così
possiamo compilare direttamente, la `sdk` la carichiamo al path `/sdk` mentre
questo progetto al path `/data` così posso fare le build tranquillamente.

```sh
docker run -u 1000:1000 -itd --name duodocker \
-v "$(pwd)":/data \
-v $HOME/git/milkv-duos-crun-podman/duo-buildroot-sdk-v2:/sdk \
milkvtech/milkv-duo:latest /bin/bash
```

## Il principio: l'SDK e' rigenerabile, non versionato

```
SDK = SDK_REF (sdk.lock)  +  sdk-patches/*.patch  +  symlink di rtos/
```

Tutti e tre i termini stanno in questo repo. L'SDK vive **fuori** (default
`~/git/duo-buildroot-sdk`) ed e' usa-e-getta: `setup-sdk.sh` lo resetta hard
a ogni run. Non ci committi nulla, non lo rebasi mai, puoi cancellarlo senza
perdere niente di tuo. E lo condividi fra progetti (Miranda, Mavis, questo).

Perche' non un submodule: ti porta i GB dentro ogni clone **e** lascia le tue
modifiche all'SDK non versionate, perche' non puoi committarle nel repo di
Sophgo. Un fork con branch `corley/duos` ha senso solo se prevedi di mantenere
patch sostanziose e durature — non tre righe.

## Struttura

```
duos-ipc/
├── sdk.lock                 URL + SHA/tag dell'SDK + target
├── sdk-patches/             ⚑ le tue modifiche AI FILE DELL'SDK
│   ├── 0001-freertos-build-ipc-task.patch
│   ├── 0002-freertos-start-ipc-task.patch
│   └── 0003-dts-reserved-memory.patch
│
├── external.desc            albero BR2_EXTERNAL: nome e descrizione
├── external.mk              raccoglie i .mk sotto package/
├── Config.in                aggancia i package al menuconfig
│
├── src/                     cio' che Buildroot compila (SITE_METHOD=local)
│   ├── shared/shared_msg.h  ⚑ unica fonte di verita' della struct
│   ├── reader.c             consumer userspace
│   └── Makefile
│
├── package/duos-ipc/        ricetta Buildroot -> /usr/bin/ipc-reader
│   ├── Config.in
│   └── duos-ipc.mk
│
├── rtos/sensor_task.c       producer FreeRTOS (symlinkato nell'SDK)
│
├── board/duos/
│   ├── rootfs-overlay/root/selftest.sh    regressione con exit code
│   └── post-build.sh
│
├── configs/duos_ipc_defconfig   frammento di config da mergiare
└── scripts/
    ├── setup-sdk.sh         clone + reset + patch + symlink. Zero passi manuali.
    ├── refresh-patches.sh   ricattura in sdk-patches/ le modifiche fatte a mano
    ├── install-rtos.sh      symlinka rtos/ nell'SDK (idempotente)
    └── deploy.sh            rebuild del solo pacchetto + scp sulla board
```

Due dettagli che spiegano il resto:

- **`shared/` sta dentro `src`.** Con `SITE_METHOD = local` Buildroot copia in
  `output/build/` soltanto la directory indicata da `_SITE`. Se l'header stesse
  fuori, ogni `-I../shared` si romperebbe alla prima build pulita. Il lato RTOS
  lo raggiunge via symlink, non viceversa.
- **I sorgenti nuovi sono symlink, le modifiche a file esistenti sono patch.**
  Il `.c` che aggiungi lo modifichi qui e ricompili subito; una riga cambiata
  nel `CMakeLists.txt` upstream non puo' che essere un diff.

## Primo giro

```sh
git init -b main && git add -A && git commit -m "chore: scaffold IPC test"

./scripts/setup-sdk.sh          # clone + reset + 3 patch + symlink

export BR2_EXTERNAL=$PWD
cd /sdk && ./build.sh milkv-duos-sd
```

Se `build.sh` non propaga `BR2_EXTERNAL` (dipende dalla revisione):

```sh
make -C buildroot BR2_EXTERNAL=<repo> milkv-duos-sd_defconfig
```

## Quando una release Sophgo rompe le patch

`setup-sdk.sh` esce con errore e ti dice quale patch e' fallita. Volutamente:
meglio qui che a runtime sulla board. Il recupero:

```sh
SKIP_PATCHES=1 ./scripts/setup-sdk.sh    # SDK pulito alla nuova ref
# ...applichi le modifiche a mano nell'SDK...
./scripts/refresh-patches.sh             # le ricattura in sdk-patches/
# rivedi il diff, riorganizza le patch, aggiorna SDK_REF in sdk.lock, committa
```

Le patch usano `git apply --3way`, quindi sopravvivono da sole a piccoli
spostamenti di contesto.

> **Nota onesta sulle 3 patch incluse.** I contesti sono scritti a mano contro
> il layout tipico dell'SDK, non generati da un albero reale. Al primo
> `setup-sdk.sh` e' probabile che una o piu' falliscano — in particolare il
> path di `main.c` (`comm/src/main.c` vs `comm/src/cvi_main.c`) e quello del
> DTS cambiano fra revisioni e varianti di board. Applica a mano, lancia
> `refresh-patches.sh`, committa: da quel momento sono verificate contro il
> **tuo** `SDK_REF` e il flusso e' a zero passi manuali.

## Ciclo di iterazione

Ricompilare l'immagine intera per una riga di C non ha senso:

```sh
make duos-ipc-rebuild && make duos-ipc-reinstall   # dentro output/ di Buildroot
./scripts/deploy.sh 192.168.42.1                   # oppure, tutto in uno
```

Sulla board:

```sh
ipc-reader -1        # one-shot, verifica il magic
ipc-reader -n 20     # 20 campioni
/root/selftest.sh    # regressione, exit code 0 = ok
```

## Le tre trappole di questa integrazione

1. **I due core non sono cache-coherent.** Lato Linux `open("/dev/mem", O_SYNC)`
   ti da' una mappatura uncached; lato RTOS, se la regione non e' uncached,
   serve un clean esplicito della D-cache dopo ogni scrittura. Sintomo tipico:
   valori corretti all'inizio e poi stantii in modo intermittente.
2. **`seq` e' la barriera di commit.** Scrivi il payload, `fence w,w`, poi
   incrementa `seq`. Il reader fa snapshot e ricontrolla `seq`: se e' cambiato,
   scarta il campione strappato. Invertire l'ordine e' silenziosamente rotto.
3. **La finestra va sottratta a Linux nel DTS** (patch 0003). Senza, il kernel
   prima o poi alloca li' sopra e il canale muore dopo minuti di funzionamento
   apparentemente corretto.

## Roadmap

- `v0-polling` — tag di baseline: polling su `seq`, doorbell ignorata.
- `v1-irq` — il reader si sveglia sull'ioctl del driver `cvi_rtos_cmdqu`
  invece di fare polling.
- `v2-ring` — ring buffer con seqlock al posto del singolo slot, per non
  perdere campioni sopra i ~100 Hz.

## Nota sui nomi dell'SDK

`request_send_to_cpu()`, `SEND_TO_CPU1`, `cmdqu_t` e l'ioctl `RTOS_CMDQU_SEND`
cambiano fra revisioni dell'SDK Sophgo. Verifica in `cvi_mailbox.h` del tuo
tree prima di dare per buono il codice del task.
