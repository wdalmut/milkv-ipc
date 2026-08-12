/*
 * ipc_cmd.c - manda un comando al core FreeRTOS, nel verso Linux -> micro.
 *
 * Il trasporto in questo verso esiste gia' tutto nell'SDK e non ci serve
 * scriverlo: `rtos_cmdqu_send()` mette il cmdqu_t nel mailbox, e lato RTOS
 * prvQueueISR() lo smista per ip_id sulle code FreeRTOS. Da userspace si passa
 * per l'ioctl del driver cvi_rtos_cmdqu.
 *
 * Il payload utile e' quello che sta in 8 byte: ip_id, cmd_id a 7 bit e un
 * param_ptr a 32 bit. Per comandi piu' grandi serve una finestra in memoria
 * condivisa, ma per "cambia soglia", "azzera", "accendi" bastano questi.
 *
 * Uso: ipc-cmd <comando> [param]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "shared/shared_msg.h"

#define CMDQU_DEV "/dev/cvi-rtos-cmdqu"
#define IPC_DEV   "/dev/duos-ipc"

/* _IOW('r', CMDQU_SEND, unsigned long) con CMDQU_SEND = 1. L'argomento e' un
 * puntatore a cmdqu_t: il driver ci fa copy_from_user. */
#define RTOS_CMDQU_SEND _IOW('r', 1, unsigned long)

/* Rispecchia enum IP_TYPE dell'SDK. */
enum {
    IP_ISP = 0, IP_VCODEC, IP_VIP, IP_VI, IP_RGN, IP_AUDIO, IP_SYSTEM, IP_CAMERA,
};

/*
 * Rispecchia enum SYS_CMD_ID dell'SDK. Scritto come enum e non come costanti
 * cablate cosi' i valori derivano dall'ordine, che e' come li assegna l'SDK: se
 * un giorno Sophgo ne inserisce uno in mezzo, questo file va riallineato ma
 * almeno la relazione e' visibile.
 */
enum {
    SYS_CMD_INFO_TRANS = 0x50,
    SYS_CMD_INFO_LINUX_INIT_DONE,
    SYS_CMD_INFO_RTOS_INIT_DONE,
    SYS_CMD_INFO_STOP_ISR,
    SYS_CMD_INFO_STOP_ISR_DONE,
    SYS_CMD_INFO_LINUX,
    SYS_CMD_INFO_RTOS,
    SYS_CMD_SYNC_TIME,
    SYS_CMD_INFO_DUMP_MSG,
    SYS_CMD_INFO_DUMP_EN,
    SYS_CMD_INFO_DUMP_DIS,
};

/* 8 byte, come il mailbox. Rispecchia struct cmdqu_t dell'SDK. */
typedef struct {
    unsigned char ip_id;
    unsigned char cmd_id : 7;
    unsigned char block  : 1;
    unsigned short resv;        /* 0 = non bloccante */
    unsigned int param_ptr;
} __attribute__((packed)) __attribute__((aligned(8))) cmdqu_t;

struct known_cmd {
    const char *name;
    unsigned char ip_id;
    unsigned char cmd_id;
    const char *help;
};

/*
 * ATTENZIONE ai nomi dell'SDK, sono invertiti rispetto all'intuizione:
 * DUMP_EN abilita la CATTURA dei messaggi in un buffer e come effetto spegne la
 * stampa sulla seriale; DUMP_DIS la riaccende. Le scorciatoie qui sotto
 * nascondono la trappola invece di propagarla.
 */
static const struct known_cmd known[] = {
    { "rtos-log-on",  IP_SYSTEM, SYS_CMD_INFO_DUMP_DIS,
      "il micro stampa sulla console seriale (manda DUMP_DIS)" },
    { "rtos-log-off", IP_SYSTEM, SYS_CMD_INFO_DUMP_EN,
      "il micro smette di stampare, cattura in buffer (manda DUMP_EN)" },
    { "sync-time",    IP_SYSTEM, SYS_CMD_SYNC_TIME,
      "sincronizza l'orologio del micro" },
};

static void usage(const char *argv0)
{
    size_t i;

    fprintf(stderr, "uso: %s <comando> [param]          (via mailbox, 8 byte)\n", argv0);
    fprintf(stderr, "     %s raw <ip_id> <cmd_id> [param]\n", argv0);
    fprintf(stderr, "     %s send <cmd> <arg> [byte...] (via finestra condivisa)\n\n",
            argv0);
    fprintf(stderr, "comandi noti:\n");
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        fprintf(stderr, "  %-14s %s\n", known[i].name, known[i].help);
    fprintf(stderr, "\n`send` scrive %zu byte su %s: il driver mette magic e seq,\n"
                    "poi suona la doorbell. Fino a %u byte di payload libero.\n",
            sizeof(host_cmd_t), IPC_DEV, CMD_DATA_LEN);
    fprintf(stderr, "\nTutto e' fire-and-forget: il mailbox non porta una"
                    " conferma.\nCe la dice solo l'effetto sul micro.\n");
}

/*
 * Comando attraverso la finestra condivisa. magic e seq NON si mettono qui: li
 * scrive il driver, cosi' un programma non puo' pubblicare un blocco malformato
 * ne' far arretrare seq. Noi riempiamo solo cmd, arg e data.
 */
static int send_window(int argc, char **argv)
{
    host_cmd_t cmd;
    ssize_t n;
    int fd, i;

    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = (uint32_t)strtoul(argv[2], NULL, 0);
    cmd.arg = (uint32_t)strtoul(argv[3], NULL, 0);

    for (i = 4; i < argc && (i - 4) < (int)CMD_DATA_LEN; i++)
        cmd.data[i - 4] = (uint8_t)strtoul(argv[i], NULL, 0);

    if (argc - 4 > (int)CMD_DATA_LEN)
        fprintf(stderr, "# %d byte in eccesso, ignorati (max %u)\n",
                argc - 4 - (int)CMD_DATA_LEN, CMD_DATA_LEN);

    fd = open(IPC_DEV, O_WRONLY);
    if (fd < 0) {
        perror("open " IPC_DEV);
        fprintf(stderr, "il modulo duos_ipc_irq e' caricato? E scrivere richiede"
                        " root: il device e' 0444.\n");
        return 1;
    }

    /* Il driver accetta esattamente sizeof(host_cmd_t): una write parziale non
     * ha senso su un messaggio che deve essere atomico. */
    n = write(fd, &cmd, sizeof(cmd));
    if (n != (ssize_t)sizeof(cmd)) {
        if (n < 0)
            perror("write " IPC_DEV);
        else
            fprintf(stderr, "write parziale: %zd byte\n", n);
        close(fd);
        return 1;
    }
    close(fd);

    printf("inviato in finestra: cmd=%u arg=0x%08x data[0..3]=%02x %02x %02x %02x\n",
           cmd.cmd, cmd.arg, cmd.data[0], cmd.data[1], cmd.data[2], cmd.data[3]);

    return 0;
}

int main(int argc, char **argv)
{
    cmdqu_t cmd;
    unsigned long param = 0;
    int fd, i, found = -1;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "send"))
        return send_window(argc, argv);

    memset(&cmd, 0, sizeof(cmd));

    if (!strcmp(argv[1], "raw")) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }
        cmd.ip_id = (unsigned char)strtoul(argv[2], NULL, 0);
        cmd.cmd_id = (unsigned char)strtoul(argv[3], NULL, 0) & 0x7f;
        if (argc > 4)
            param = strtoul(argv[4], NULL, 0);
    } else {
        for (i = 0; i < (int)(sizeof(known) / sizeof(known[0])); i++) {
            if (!strcmp(argv[1], known[i].name)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            fprintf(stderr, "comando sconosciuto: %s\n\n", argv[1]);
            usage(argv[0]);
            return 2;
        }
        cmd.ip_id = known[found].ip_id;
        cmd.cmd_id = known[found].cmd_id;
        if (argc > 2)
            param = strtoul(argv[2], NULL, 0);
    }

    cmd.block = 0;          /* fire & forget */
    cmd.resv = 0;           /* mstime 0 = non bloccante */
    cmd.param_ptr = (unsigned int)param;

    fd = open(CMDQU_DEV, O_RDWR);
    if (fd < 0) {
        perror("open " CMDQU_DEV);
        fprintf(stderr, "il modulo cv181x_rtos_cmdqu e' caricato?"
                        " lsmod | grep rtos_cmdqu\n");
        return 1;
    }

    if (ioctl(fd, RTOS_CMDQU_SEND, &cmd) < 0) {
        perror("ioctl RTOS_CMDQU_SEND");
        close(fd);
        return 1;
    }
    close(fd);

    printf("inviato: ip_id=%u cmd_id=0x%02x param=0x%08x\n",
           cmd.ip_id, cmd.cmd_id, cmd.param_ptr);

    return 0;
}
