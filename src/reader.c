/*
 * reader.c - consumer Linux della finestra di memoria condivisa col FreeRTOS.
 *
 * Modo di default (v1-irq): si dorme in poll() su /dev/duos-ipc e si legge la
 * finestra solo quando il doorbell suona. Il campanello passa dal mailbox, il
 * payload dalla memoria condivisa: sono due canali distinti e restano tali.
 *
 * Il polling su `seq` resta disponibile con -P, perche' serve come termine di
 * confronto e perche' funziona anche su un'immagine senza il modulo.
 *
 * Uso: ipc-reader [-n N] [-i MS] [-P] [-1]
 *   -n N    esce dopo N messaggi (0 = infinito, default)
 *   -i MS   intervallo di polling in ms, solo con -P (default 1)
 *   -P      polling su seq invece dell'attesa sul doorbell
 *   -1      one-shot: stampa lo stato corrente ed esce
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>

#include "shared/shared_msg.h"

#define DOORBELL_DEV    "/dev/duos-ipc"
#define DOORBELL_WAIT_MS 1000

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

/*
 * Le due perdite vanno tenute distinte: un salto di `seq` dice che il producer
 * ha sovrascritto campioni che non abbiamo letto, un salto di `bell` dice che le
 * notifiche sono state accorpate. Sono guasti diversi.
 */
static void print_sample(const sensor_msg_t *s, uint32_t lost_seq, uint32_t lost_bell)
{
    printf("seq=%-8u T=%7.3f C  vib=%-6u ts=%llu us  drops=%u",
           s->seq, s->temp_mC / 1000.0, s->vib_rms,
           (unsigned long long)s->ts_us, s->drops);

    if (lost_seq)
        printf("  [SEQ -%u]", lost_seq);
    if (lost_bell)
        printf("  [BELL -%u]", lost_bell);

    putchar('\n');
    fflush(stdout);
}

/*
 * Snapshot piu' ricontrollo di seq: se il producer ha scritto mentre leggevamo,
 * il campione e' strappato e va scartato. Ritorna 0 se il campione non e'
 * utilizzabile.
 */
static int snapshot(volatile sensor_msg_t *m, sensor_msg_t *out)
{
    uint32_t seq;

    if (m->magic != MSG_MAGIC)
        return 0;

    seq = m->seq;
    memcpy(out, (const void *)m, sizeof(*out));

    return m->seq == seq;
}

/* Attesa sul doorbell. Ritorna 0 a fine corsa. */
static int run_irq(int dfd, volatile sensor_msg_t *m, long limit)
{
    uint32_t last_seq = 0, last_bell = 0;
    int first = 1, silent = 0, stale = 0;
    long count = 0;

    while (running) {
        struct pollfd pfd = { .fd = dfd, .events = POLLIN, .revents = 0 };
        sensor_msg_t snap;
        uint32_t bell, lost_bell, lost_seq;
        int r = poll(&pfd, 1, DOORBELL_WAIT_MS);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        if (r == 0) {
            /* Nessun doorbell: e' il sintomo di un producer fermo. Lo diciamo
             * una volta sola, per non trasformare la diagnosi in spam. */
            if (!silent) {
                fprintf(stderr, "# nessun doorbell da %d ms: producer fermo?\n",
                        DOORBELL_WAIT_MS);
                silent = 1;
            }
            continue;
        }
        silent = 0;

        if (read(dfd, &bell, sizeof(bell)) != (ssize_t)sizeof(bell)) {
            if (errno == EINTR)
                continue;
            perror("read " DOORBELL_DEV);
            return 1;
        }

        if (!snapshot(m, &snap))
            continue;

        if (first) {
            lost_bell = 0;
            lost_seq = 0;
            first = 0;
        } else {
            lost_bell = bell - last_bell - 1;
            lost_seq = snap.seq - last_seq - 1;

            /*
             * Doorbell suonata ma payload fermo. Non e' un campione: stamparlo
             * farebbe sembrare traffico un canale congelato, che e' proprio il
             * modo in cui questo guasto si e' presentato la prima volta.
             * Lo si segnala una volta e si tace, come per il timeout.
             */
            if (snap.seq == last_seq) {
                if (!stale) {
                    fprintf(stderr, "# doorbell con payload fermo a seq=%u:"
                            " producer bloccato dopo la notifica?\n", snap.seq);
                    stale = 1;
                }
                last_bell = bell;
                continue;
            }
            stale = 0;
        }

        last_bell = bell;
        last_seq = snap.seq;

        print_sample(&snap, lost_seq, lost_bell);

        if (limit && ++count >= limit)
            break;
    }

    return 0;
}

/* Polling su seq, senza doorbell. Ritorna 0 a fine corsa. */
static int run_poll(volatile sensor_msg_t *m, long limit, long interval_ms)
{
    uint32_t last_seq = 0;
    int first = 1;
    long count = 0;

    while (running) {
        sensor_msg_t snap;

        if (snapshot(m, &snap) && snap.seq != last_seq) {
            if (first) {
                first = 0;
            } else {
                print_sample(&snap, snap.seq - last_seq - 1, 0);
                if (limit && ++count >= limit)
                    break;
            }
            last_seq = snap.seq;
        }

        usleep((useconds_t)(interval_ms * 1000));
    }

    return 0;
}

int main(int argc, char **argv)
{
    long limit = 0;
    long interval_ms = 1;
    int one_shot = 0;
    int force_poll = 0;
    int opt, ret;

    while ((opt = getopt(argc, argv, "n:i:P1h")) != -1) {
        switch (opt) {
        case 'n': limit = strtol(optarg, NULL, 0); break;
        case 'i': interval_ms = strtol(optarg, NULL, 0); break;
        case 'P': force_poll = 1; break;
        case '1': one_shot = 1; break;
        default:
            fprintf(stderr, "uso: %s [-n N] [-i MS] [-P] [-1]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /*
     * O_SYNC e' cio' che ci fa ottenere una mappatura NON cacheable su
     * /dev/mem: i due core non sono cache-coherent, senza questo leggeresti
     * dati stantii dalla D-cache.
     */
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem (serve root, e CONFIG_STRICT_DEVMEM disabilitato)");
        return 1;
    }

    void *p = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, fd, SHM_PHYS_ADDR);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    volatile sensor_msg_t *m = (volatile sensor_msg_t *)p;
    printf("# mappato 0x%08x (%u B), sizeof(sensor_msg_t)=%zu\n",
           SHM_PHYS_ADDR, SHM_SIZE, sizeof(sensor_msg_t));

    if (one_shot) {
        /*
         * Lo snapshot va preso PRIMA di munmap(): dopo, quell'indirizzo non e'
         * piu' mappato e dereferenziarlo e' un SIGSEGV.
         */
        sensor_msg_t snap;
        memcpy(&snap, (const void *)m, sizeof(snap));

        printf("magic=0x%08x seq=%u temp=%d mC vib=%u ts=%llu drops=%u\n",
               snap.magic, snap.seq, snap.temp_mC, snap.vib_rms,
               (unsigned long long)snap.ts_us, snap.drops);
        munmap(p, SHM_SIZE);
        close(fd);
        return (snap.magic == MSG_MAGIC) ? 0 : 1;
    }

    int dfd = -1;
    if (!force_poll) {
        dfd = open(DOORBELL_DEV, O_RDONLY);
        if (dfd < 0) {
            /*
             * Ripiego invece di morire: la stessa immagine deve funzionare
             * anche senza il modulo caricato. Ma lo si dice, perche' un
             * fallback silenzioso e' peggio di un errore.
             */
            fprintf(stderr, "# %s non disponibile (%s): ripiego sul polling\n",
                    DOORBELL_DEV, strerror(errno));
            force_poll = 1;
        }
    }

    if (force_poll) {
        printf("# modo polling, intervallo %ld ms\n", interval_ms);
        ret = run_poll(m, limit, interval_ms);
    } else {
        printf("# modo doorbell su %s\n", DOORBELL_DEV);
        ret = run_irq(dfd, m, limit);
        close(dfd);
    }

    munmap(p, SHM_SIZE);
    close(fd);

    return ret;
}
