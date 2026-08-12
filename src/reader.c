/*
 * reader.c - consumer Linux del canale IPC col core FreeRTOS.
 *
 * Modo di default: read() su /dev/duos-ipc. Blocca finche' il C906 non pubblica
 * un campione nuovo e restituisce la struct gia' verificata dal driver. Non
 * serve root: il campanello, il seqlock e la mappatura non cacheable stanno
 * tutti dall'altra parte del device.
 *
 * Uso: ipc-reader [-n N] [-i MS] [-P] [-1] [--devmem]
 *   -n N      esce dopo N messaggi (0 = infinito, default)
 *   -P        polling non bloccante sul device invece dell'attesa
 *   -i MS     intervallo del polling in ms, solo con -P (default 1)
 *   -1        one-shot: stampa il campione corrente ed esce
 *   --devmem  scavalca il modulo e legge la finestra da /dev/mem (serve root)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <sys/mman.h>

#include "shared/shared_msg.h"

#define IPC_DEV          "/dev/duos-ipc"
#define DOORBELL_WAIT_MS 1000

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static void print_sample(const sensor_msg_t *s, uint32_t lost)
{
    printf("seq=%-8u T=%7.3f C  vib=%-6u ts=%llu us  drops=%u%s\n",
           s->seq, s->temp_mC / 1000.0, s->vib_rms,
           (unsigned long long)s->ts_us, s->drops,
           lost ? "  [PERSI]" : "");
    fflush(stdout);
}

static void explain_read_error(int err)
{
    switch (err) {
    case ENODATA:
        fprintf(stderr, "# magic assente: il task FreeRTOS non ha ancora"
                " pubblicato. Gira? L'indirizzo nel DTS combacia?\n");
        break;
    case EAGAIN:
        fprintf(stderr, "# snapshot sempre strappato: il producer scrive molto"
                " piu' in fretta del previsto?\n");
        break;
    default:
        errno = err;
        perror("read " IPC_DEV);
    }
}

/* Una lettura dal device. Ritorna 1 se ha un campione, 0 se riprovare,
 * -1 se e' un errore su cui fermarsi. */
static int read_sample(int fd, sensor_msg_t *out)
{
    ssize_t n = read(fd, out, sizeof(*out));

    if (n == (ssize_t)sizeof(*out))
        return 1;

    if (n < 0) {
        if (errno == EINTR)
            return 0;
        if (errno == EAGAIN)       /* O_NONBLOCK: niente di nuovo */
            return 0;
        explain_read_error(errno);
        return -1;
    }

    fprintf(stderr, "# read parziale: %zd byte invece di %zu\n",
            n, sizeof(*out));
    return -1;
}

/*
 * Attesa sul device. Con nonblock il poll() salta e si gira a vuoto: serve
 * come termine di confronto per misurare quanto costa il polling.
 */
static int run_device(int fd, long limit, int nonblock, long interval_ms)
{
    uint32_t last_seq = 0;
    int first = 1, silent = 0;
    long count = 0;

    while (running) {
        sensor_msg_t s;
        uint32_t lost;
        int r;

        if (!nonblock) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
            int p = poll(&pfd, 1, DOORBELL_WAIT_MS);

            if (p < 0) {
                if (errno == EINTR)
                    continue;
                perror("poll");
                return 1;
            }
            if (p == 0) {
                /* Nessun campione: e' il sintomo di un producer fermo. Lo si
                 * dice una volta sola, per non trasformare la diagnosi in
                 * spam. */
                if (!silent) {
                    fprintf(stderr, "# nessun campione da %d ms:"
                            " producer fermo?\n", DOORBELL_WAIT_MS);
                    silent = 1;
                }
                continue;
            }
            silent = 0;
        }

        r = read_sample(fd, &s);
        if (r < 0)
            return 1;
        if (r == 0) {
            if (nonblock)
                usleep((useconds_t)(interval_ms * 1000));
            continue;
        }

        lost = first ? 0 : s.seq - last_seq - 1;
        first = 0;
        last_seq = s.seq;

        print_sample(&s, lost);

        if (limit && ++count >= limit)
            break;
    }

    return 0;
}

/*
 * Percorso di diagnostica: legge la finestra scavalcando il modulo. E' l'unico
 * modo per distinguere "il producer e' morto" da "il modulo e' rotto", ed e'
 * anche il motivo per cui non e' il default: richiede root e un kernel senza
 * CONFIG_STRICT_DEVMEM, cioe' la capacita' di leggere tutta la memoria fisica.
 *
 * Qui il seqlock va rifatto a mano, perche' il driver non e' nel percorso.
 */
static int snapshot_devmem(volatile sensor_msg_t *m, sensor_msg_t *out)
{
    uint32_t seq;

    if (m->magic != MSG_MAGIC)
        return 0;

    seq = m->seq;
    memcpy(out, (const void *)m, sizeof(*out));

    return m->seq == seq;
}

static int run_devmem(long limit, long interval_ms, int one_shot)
{
    uint32_t last_seq = 0;
    int first = 1, ret = 0;
    long count = 0;
    void *p;
    volatile sensor_msg_t *m;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem (serve root, e CONFIG_STRICT_DEVMEM disabilitato)");
        return 1;
    }

    p = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, fd, SHM_PHYS_ADDR);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    m = (volatile sensor_msg_t *)p;

    printf("# --devmem: mappato 0x%08x (%u B)\n", SHM_PHYS_ADDR, SHM_SIZE);

    if (one_shot) {
        /* Lo snapshot va preso PRIMA di munmap(): dopo, quell'indirizzo non e'
         * piu' mappato e dereferenziarlo e' un SIGSEGV. */
        sensor_msg_t s;

        memcpy(&s, (const void *)m, sizeof(s));
        printf("magic=0x%08x seq=%u temp=%d mC vib=%u ts=%llu drops=%u\n",
               s.magic, s.seq, s.temp_mC, s.vib_rms,
               (unsigned long long)s.ts_us, s.drops);
        ret = (s.magic == MSG_MAGIC) ? 0 : 1;
        goto out;
    }

    while (running) {
        sensor_msg_t s;

        if (snapshot_devmem(m, &s) && s.seq != last_seq) {
            if (!first) {
                print_sample(&s, s.seq - last_seq - 1);
                if (limit && ++count >= limit)
                    break;
            }
            first = 0;
            last_seq = s.seq;
        }

        usleep((useconds_t)(interval_ms * 1000));
    }

out:
    munmap(p, SHM_SIZE);
    close(fd);

    return ret;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "uso: %s [-n N] [-i MS] [-P] [-1] [--devmem]\n"
            "  -n N      esce dopo N messaggi (0 = infinito)\n"
            "  -P        polling non bloccante invece dell'attesa\n"
            "  -i MS     intervallo del polling, solo con -P\n"
            "  -1        one-shot\n"
            "  --devmem  legge da /dev/mem scavalcando il modulo (root)\n",
            argv0);
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "devmem", no_argument, NULL, 'm' },
        { "help",   no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    long limit = 0;
    long interval_ms = 1;
    int one_shot = 0, force_poll = 0, devmem = 0;
    int opt, fd, ret;

    while ((opt = getopt_long(argc, argv, "n:i:P1h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'n': limit = strtol(optarg, NULL, 0); break;
        case 'i': interval_ms = strtol(optarg, NULL, 0); break;
        case 'P': force_poll = 1; break;
        case '1': one_shot = 1; break;
        case 'm': devmem = 1; break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (devmem)
        return run_devmem(one_shot ? 0 : limit, interval_ms, one_shot);

    fd = open(IPC_DEV, force_poll ? (O_RDONLY | O_NONBLOCK) : O_RDONLY);
    if (fd < 0) {
        /*
         * Niente ripiego automatico su /dev/mem: un fallback silenzioso ti fa
         * credere che il canale funzioni mentre stai usando la strada vecchia,
         * con i privilegi che credevi di aver tolto. Se serve, si chiede.
         */
        fprintf(stderr, "# %s non disponibile (%s).\n", IPC_DEV, strerror(errno));
        fprintf(stderr, "#   Il modulo duos_ipc_irq e' caricato? lsmod | grep duos\n");
        fprintf(stderr, "#   Per leggere comunque la finestra: %s --devmem\n",
                argv[0]);
        return 1;
    }

    printf("# %s, sizeof(sensor_msg_t)=%zu\n", IPC_DEV, sizeof(sensor_msg_t));

    if (one_shot) {
        sensor_msg_t s;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

        if (!force_poll && poll(&pfd, 1, DOORBELL_WAIT_MS) == 0) {
            fprintf(stderr, "# nessun campione in %d ms: producer fermo?\n",
                    DOORBELL_WAIT_MS);
            close(fd);
            return 1;
        }

        ret = read_sample(fd, &s) == 1 ? 0 : 1;
        if (ret == 0)
            printf("magic=0x%08x seq=%u temp=%d mC vib=%u ts=%llu drops=%u\n",
                   s.magic, s.seq, s.temp_mC, s.vib_rms,
                   (unsigned long long)s.ts_us, s.drops);
        close(fd);
        return ret;
    }

    printf("# modo %s\n", force_poll ? "polling non bloccante" : "attesa su poll()");
    ret = run_device(fd, limit, force_poll, interval_ms);
    close(fd);

    return ret;
}
