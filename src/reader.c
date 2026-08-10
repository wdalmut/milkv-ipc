/*
 * reader.c - consumer Linux della finestra di memoria condivisa col FreeRTOS.
 *
 * Modo di default: polling su `seq`. Sufficiente per validare barriere e
 * cache policy prima di passare all'ioctl del driver cvi_rtos_cmdqu.
 *
 * Uso: ipc-reader [-n N] [-i MS] [-1]
 *   -n N    esce dopo N messaggi (0 = infinito, default)
 *   -i MS   intervallo di polling in ms (default 1)
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
#include <sys/mman.h>

#include "shared/shared_msg.h"

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

int main(int argc, char **argv)
{
    long limit = 0;
    long interval_ms = 1;
    int one_shot = 0;
    int opt;

    while ((opt = getopt(argc, argv, "n:i:1h")) != -1) {
        switch (opt) {
        case 'n': limit = strtol(optarg, NULL, 0); break;
        case 'i': interval_ms = strtol(optarg, NULL, 0); break;
        case '1': one_shot = 1; break;
        default:
            fprintf(stderr, "uso: %s [-n N] [-i MS] [-1]\n", argv[0]);
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
        printf("magic=0x%08x seq=%u temp=%d mC vib=%u ts=%llu drops=%u\n",
               m->magic, m->seq, m->temp_mC, m->vib_rms,
               (unsigned long long)m->ts_us, m->drops);
        munmap(p, SHM_SIZE);
        close(fd);
        return (m->magic == MSG_MAGIC) ? 0 : 1;
    }

    uint32_t last = 0;
    int first = 1;
    long count = 0;

    while (running) {
        if (m->magic == MSG_MAGIC) {
            uint32_t seq = m->seq;

            if (first) {
                last = seq;
                first = 0;
            } else if (seq != last) {
                /*
                 * Snapshot poi ricontrollo di seq: se il producer ha scritto
                 * mentre leggevamo, il campione e' strappato -> lo scartiamo.
                 */
                sensor_msg_t snap;
                memcpy(&snap, (const void *)m, sizeof(snap));

                if (m->seq == seq) {
                    uint32_t lost = seq - last - 1;
                    printf("seq=%-8u T=%7.3f C  vib=%-6u ts=%llu us"
                           "  drops=%u%s\n",
                           snap.seq, snap.temp_mC / 1000.0, snap.vib_rms,
                           (unsigned long long)snap.ts_us, snap.drops,
                           lost ? "  [MISSED]" : "");
                    fflush(stdout);
                    last = seq;
                    if (limit && ++count >= limit)
                        break;
                }
            }
        }
        usleep((useconds_t)(interval_ms * 1000));
    }

    munmap(p, SHM_SIZE);
    close(fd);
    return 0;
}
