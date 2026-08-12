# duos-ipc

Banco di prova per il passaggio di una struct dal core **FreeRTOS** (C906 small)
a **Linux** sul Milk-V Duo S / SG2000, via memoria condivisa + doorbell sul
mailbox.

Il mailbox del SG2000 trasporta solo 8 byte (`cmdqu_t`): non ci passi una
struct. Il pattern e' **payload in memoria condivisa, notifica sul mailbox**.

Stato: `v1-irq`. Il reader dorme in `poll()` e si sveglia sull'interrupt del
mailbox — 20 campioni a 10 Hz in 1.95 s reali con `sys 0.00s`. Verificato su
`duo-buildroot-sdk-v2` a `v2.0.1`.

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
patch sostanziose e durature — non cinque righe.

C'e' una quarta cosa, che non e' una patch: il defconfig Buildroot della board.
Lo modifica `enable-package.sh` invece di `sdk-patches/`, perche' una patch
porta con se' il contesto esatto di quel file e si rompe appena Milk-V lo tocca,
mentre lo script cerca per chiave ed e' idempotente. In piu' `BR2_ROOTFS_OVERLAY`
li' e' **gia' impostata**: va appesa, e un `cat >>` la sostituirebbe svuotando il
rootfs della board.

## Struttura

```
duos-ipc/
├── sdk.lock                 URL + tag dell'SDK + target
├── sdk-patches/             ⚑ le tue modifiche AI FILE DELL'SDK
│   ├── 0001-freertos-build-ipc-task.patch      compila task/ipc/ dentro "comm"
│   ├── 0002-freertos-start-ipc-task.patch      avvia il task da main_cvirtos()
│   ├── 0003-dts-reserved-memory.patch          sottrae la finestra a Linux
│   ├── 0004-freertos-mailbox-full-not-fatal.patch
│   └── 0005-buildroot-propagate-br2-external.patch
│
├── external.desc            albero BR2_EXTERNAL: nome e descrizione
├── external.mk              raccoglie i .mk sotto package/
├── Config.in                aggancia i package al menuconfig
│
├── src/                     cio' che Buildroot compila (SITE_METHOD=local)
│   ├── shared/shared_msg.h  ⚑ unica fonte di verita' della struct
│   ├── reader.c             consumer userspace
│   ├── kmod/duos_ipc_irq.c  modulo doorbell -> /dev/duos-ipc
│   └── Makefile
│
├── package/duos-ipc/        ricetta Buildroot -> /usr/bin/ipc-reader + .ko
│   ├── Config.in
│   └── duos-ipc.mk
│
├── rtos/sensor_task.c       producer FreeRTOS (symlinkato nell'SDK)
│
├── board/duos/
│   ├── rootfs-overlay/root/selftest.sh          regressione con exit code
│   ├── rootfs-overlay/etc/init.d/S99zduos-ipc   carica il modulo al boot
│   └── post-build.sh
│
├── configs/duos_ipc_defconfig   frammento mergiato da enable-package.sh
└── scripts/
    ├── setup-sdk.sh         clone + reset + patch + symlink + defconfig
    ├── enable-package.sh    attiva il package nel defconfig (idempotente)
    ├── refresh-patches.sh   ricattura in sdk-patches/ le modifiche fatte a mano
    ├── install-rtos.sh      symlinka rtos/ nell'SDK (idempotente)
    └── deploy.sh            rebuild del solo pacchetto + scp sulla board
```

Tre dettagli che spiegano il resto:

- **Tutto cio' che si compila sta dentro `src/`**, header condiviso e modulo
  kernel compresi. Con `SITE_METHOD = local` Buildroot copia in `output/build/`
  soltanto la directory indicata da `_SITE`: quello che sta fuori non esiste. Il
  lato RTOS raggiunge l'header via symlink, non viceversa.
- **I sorgenti nuovi sono symlink, le modifiche a file esistenti sono patch.**
  Il `.c` che aggiungi lo modifichi qui e ricompili subito; una riga cambiata
  nel `CMakeLists.txt` upstream non puo' che essere un diff.
- **`S99zduos-ipc` ha quella `z` per un motivo.** `rcS` esegue `/etc/init.d/S??*`
  in ordine alfabetico, e i moduli cvitek li carica `S99user`. Chiamandolo
  `S99duos-ipc` girava *prima*, non trovava `cv181x_rtos_cmdqu` e si fermava.

## Come funziona il canale

Due percorsi distinti, ed e' voluto: il mailbox trasporta 8 byte, quindi porta
solo il campanello.

```
sensor_task (C906)                     linux
  publish() ─────► shared memory 0x8FE00000 ─────► reader: snapshot + mmap
  doorbell()                                              ▲
    │ cmdqu block=0, ip_id=IP_SYSTEM, cmd_id=0x40         │ poll() + read()
    ▼                                                     │
  E_QUEUE_CMDQU ──► mailbox ──► rtos_irq_handler ──► duos_ipc_irq.ko
                                  (ramo handler)     bell++ ; wake_up
```

**Perche' serve un modulo kernel.** Userspace non ha modo di bloccarsi su un
messaggio non sollecitato: `RTOS_CMDQU_SEND_WAIT` e' manda-e-attendi-risposta,
non "aspetta". L'unico gancio per un push e' un handler in-kernel registrato con
`request_rtos_irq()`, ed e' quello che il modulo occupa per `IP_SYSTEM`.

**Perche' `block = 0`.** L'ISR di `cvi_rtos_cmdqu` smista in tre modi: con
`block = 1` sveglia chi attende in `SEND_WAIT`; altrimenti chiama l'handler
registrato per quell'`ip_id`; altrimenti stampa `error ip=.. cmd=..`. Con
`block = 0` finiamo nel ramo di mezzo. Quel messaggio d'errore e' quindi
l'osservabile che dice che la doorbell suona ma nessuno l'ha agganciata: se
compare, il modulo non e' caricato.

`/dev/duos-ipc` espone un **contatore monotono** di doorbell, non una coda di
eventi: la finestra condivisa e' un singolo slot che viene sovrascritto, quindi
notifiche arretrate punterebbero tutte allo stesso dato. Il reader confronta col
valore precedente e ricava da se' quante ne ha perse. Lo stato e' per file
descriptor, cosi' due reader si svegliano entrambi.

## Primo giro

```sh
REPO_PATH=/data ./scripts/setup-sdk.sh /sdk   # patch + symlink + defconfig

export BR2_EXTERNAL=/data                     # il path COME LO VEDE la build
cd /sdk && ./build.sh milkv-duos-musl-riscv64-sd
```

**Due path, la stessa regola: contano come li vede chi compila.** Dentro il
container questo repo e' `/data`, non `~/git/milkv-duos-ipc`, e i due nomi
falliscono in modi diversi:

- `BR2_EXTERNAL` sbagliato non da' errore. Da' un'immagine senza il pacchetto,
  e te ne accorgi solo sulla board quando `ipc-reader` non c'e';
- `REPO_PATH` sbagliato lo scopri subito, ma il messaggio non aiuta: i symlink
  in `task/ipc/` sono **assoluti**, e se puntano all'host CMake si ferma su
  `Cannot find source file: .../task/ipc/sensor_task.c`. Il file c'e', e'
  il link che non risolve. Non esiste un path relativo che vada bene da
  entrambi i lati, perche' `/sdk` e `/data` non hanno una radice comune con
  `~/git/`.

`REPO_PATH` si ricorda in `.repo-path`: serve solo la prima volta, e i run
successivi di `setup-sdk.sh` non riportano i symlink all'host annullando la
scelta in silenzio. Per tornare indietro lo si passa esplicito.

Se compili sull'host e basta, `./scripts/setup-sdk.sh` senza variabili fa la
cosa giusta e verifica che i symlink risolvano.

I nomi delle board stanno in `device/` dell'SDK: `milkv-duos-musl-riscv64-sd`
(RISC-V, la variante di questo repo), `milkv-duos-glibc-arm64-sd`, piu' le due
emmc.

## Ciclo di iterazione

Ricompilare l'immagine intera per una riga di C non ha senso:

```sh
make duos-ipc-rebuild && make duos-ipc-reinstall   # dentro output/ di Buildroot
./scripts/deploy.sh 192.168.42.1                   # oppure, tutto in uno
```

> **Buildroot non si accorge che hai toccato `src/`.** Con
> `SITE_METHOD = local` l'rsync dei sorgenti avviene **una volta sola** e poi e'
> protetto da `.stamp_rsynced`. Da quel momento `build.sh` salta il pacchetto in
> silenzio: nessun avviso, nessun errore, e l'immagine esce con il codice
> vecchio. Vale anche per i file *nuovi* — e' cosi' che `src/kmod/` e' rimasto
> fuori da un'immagine intera.
>
> Il metodo garantito e' cancellare la build dir del pacchetto:
>
> ```sh
> rm -rf <sdk>/buildroot/output/<board>/build/duos-ipc-1.0
> ```
>
> `duos-ipc-rebuild` e' piu' leggero ma **verifica che rifaccia l'rsync** sulla
> tua revisione di Buildroot: confronta la data di
> `build/duos-ipc-1.0/reader.c` con quella di `src/reader.c`. Se non combacia,
> hai appena costruito il codice di ieri.
>
> Le modifiche al `rootfs-overlay/` non sono soggette a questo: quelle entrano
> a ogni build. E' un'asimmetria che confonde, perche' vedi arrivare l'init
> script aggiornato e concludi che sia arrivato tutto.

Il modulo kernel non usa l'infrastruttura `kernel-module` di Buildroot: quella
presuppone che sia Buildroot a costruire il kernel, mentre qui lo costruisce
l'SDK. Il package invoca il Makefile del kernel a mano, e due dettagli lo
rendono meno banale di quanto sembri: i simboli di `cv181x_rtos_cmdqu` non stanno
nel `Module.symvers` del kernel (serve `KBUILD_EXTRA_SYMBOLS`), e il `Makefile`
stub dentro la output dir del kernel contiene il path assoluto del container che
l'ha compilata, quindi si entra dai sorgenti con `O=`.

Sulla board:

```sh
ipc-reader -1        # one-shot, verifica il magic
ipc-reader -n 20     # 20 campioni, attesa sul doorbell
ipc-reader -n 20 -P  # gli stessi, in polling: termine di confronto
/root/selftest.sh    # regressione, exit code 0 = ok
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

`refresh-patches.sh` produce un `9999-local-changes-*.patch` **cumulativo**: e'
materiale grezzo da rivedere e spezzare nella serie numerata, non una patch da
tenere. Lasciarlo li' e' peggio che inutile — applica senza rumore solo perche'
`--3way` tollera il gia'-applicato, e alla prima modifica a una delle altre
entra in conflitto.

## Le trappole di questa integrazione

1. **I due core non sono cache-coherent.** Lato Linux `open("/dev/mem", O_SYNC)`
   ti da' una mappatura uncached; lato RTOS serve un clean esplicito della
   D-cache dopo ogni scrittura (`flush_dcache_range`). Sintomo tipico: valori
   corretti all'inizio e poi stantii in modo intermittente.
2. **`seq` e' la barriera di commit.** Scrivi il payload, `fence w,w`, poi
   incrementa `seq`. Il reader fa snapshot e ricontrolla `seq`: se e' cambiato,
   scarta il campione strappato. Invertire l'ordine e' silenziosamente rotto.
3. **La finestra va sottratta a Linux nel DTS** (patch 0003). Senza, il kernel
   prima o poi alloca li' sopra e il canale muore dopo minuti di funzionamento
   apparentemente corretto.
4. **Il mailbox ha 8 slot e l'RTOS parte prima di Linux.** Il C906 e' su a 1,3 s,
   il driver Linux del mailbox fa probe a 3,8 s: a 10 Hz gli slot si esauriscono
   in meno di un secondo, con nessuno che li drena. Upstream gestiva la cosa con
   un `return -1` **da una funzione task `void`**, e il port RISC-V mette `x0`
   come indirizzo di ritorno del task: l'RTOS saltava a 0 e moriva. La patch 0004
   lo rende non fatale.
5. **Un canale morto sembra vivo.** Quando l'RTOS e' andato giu', la finestra
   resta con `magic` valido e payload coerente: qualunque lettura singola passa.
   Solo due letture distanziate mostrano che `seq` non avanza — per questo il
   punto 2 di `selftest.sh` esiste, ed e' la lacuna che aveva lasciato passare la
   trappola 4.

## Limite noto: `/dev/mem`

Il payload passa ancora da `/dev/mem`, quindi il reader vuole root **e** un
kernel senza `CONFIG_STRICT_DEVMEM`. Non stai aprendo una finestra sui tuoi
4 KiB: stai tenendo aperta la capacita' di leggere tutta la memoria fisica.

Su un banco di prova si sopravvive. Il passo successivo e' far uscire i dati dal
device: `read()` restituisce la struct, il seqlock si sposta nel kernel dove
nessun consumatore puo' piu' sbagliarlo, e il canale diventa leggibile da Go o
Python senza `unsafe`.

## Roadmap

- `v0-polling` ✔ — baseline: polling su `seq`, doorbell spenta. Il punto a cui
  tornare quando qualcosa in v1 si rompe.
- `v1-irq` ✔ — il reader si sveglia sull'interrupt del mailbox tramite
  `duos_ipc_irq` e `/dev/duos-ipc`.
- **dati dal device** — `read()` restituisce la struct, via `/dev/mem` e root.
- `v2-ring` — ring buffer con seqlock al posto del singolo slot, per non
  perdere campioni sopra i ~100 Hz.

## Nota sui nomi dell'SDK

I nomi cambiano fra revisioni Sophgo, e la v2 ne ha gia' cambiati parecchi
rispetto a quanto trovi in giro:

- **`request_send_to_cpu()` non esiste.** Per mandare un messaggio a Linux si
  accoda un `cmdqu_t` su `E_QUEUE_CMDQU`: `prvCmdQuRunTask()` non riconosce il
  `cmd_id`, cade nel ramo `default` ed e' quello che scrive nel mailbox.
- **Il punto di innesto non e' `main()`** ma `main_cvirtos()`, in
  `freertos/cvitek/task/comm/src/riscv64/comm_main.c`. `task/main/src/main.c` si
  limita a chiamarlo.
- **`cmd_id` e' un bitfield a 7 bit**: sotto 128, o lo tronchi in silenzio.

Verifica in `cvi_mailbox.h` e `rtos_cmdqu.h` del tuo tree prima di dare per
buono il codice del task.
