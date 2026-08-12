/*
 * sensor_task.c - producer lato FreeRTOS (C906 small core, SG2000).
 *
 * NON si compila standalone: va innestato nell'SDK Sophgo sotto
 * freertos/cvitek/task/ipc/. Ci pensa ../scripts/install-rtos.sh.
 *
 * Verificato contro duo-buildroot-sdk-v2 @ v2.0.1 (vedi sdk.lock).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Header dell'SDK: i nomi variano fra revisioni, verifica nel tuo tree. */
#include "rtos_cmdqu.h"     /* cmdqu_t, enum IP_TYPE                     */
#include "comm.h"           /* main_GetMODHandle(), E_QUEUE_CMDQU        */
#include "printf.h"

#include "shared_msg.h"

/*
 * Log del core piccolo sulla console seriale.
 *
 * ATTENZIONE AI NOMI, sono controintuitivi: dump_uart_enable() abilita la
 * CATTURA dei messaggi in un buffer in RAM (che Linux poi legge con
 * SYS_CMD_INFO_DUMP_MSG) e come effetto collaterale mette uart_putc_enable = 0,
 * cioe' SPEGNE la stampa sulla seriale. Quella che fa parlare il core e'
 * dump_uart_disable().
 *
 * Al boot dump_uart_init() chiama enable() se transfer_config.dump_print_enable
 * e' settato, e su questa board lo e': e' il motivo della riga
 * "dump_print_enable & log will not print" che vedi in console.
 *
 * Il costo: RTOS e Linux condividono la UART senza arbitraggio, quindi due
 * printf simultanei si intrecciano a meta' riga. Si vede gia' nel log di boot.
 * Con una riga sola all'avvio e' trascurabile; con un heartbeat frequente no.
 */
#define IPC_RTOS_LOG   1
#define IPC_LOG_EVERY  100 /* > 0: una riga ogni N pubblicazioni (100 = 10 s) */

#if IPC_RTOS_LOG
/* Dichiarata in driver/uart/include/dump_uart.h, che non e' sulla include path
 * della libreria "comm". */
extern void dump_uart_disable(void);
#endif

/*
 * La finestra e' sottratta a Linux nel DTS (vedi patch 0003), quindi la
 * indirizziamo direttamente. volatile: il compilatore non deve accorpare
 * o riordinare questi store.
 */
static volatile sensor_msg_t *const g_shm =
        (volatile sensor_msg_t *)(uintptr_t)SHM_PHYS_ADDR;

/*
 * Dichiarata in arch/riscv64/include/arch_helpers.h. La ripetiamo qui invece
 * di includere l'header perche' non e' fra quelli installati in
 * install/include/ visti dalla libreria "comm".
 */
extern void flush_dcache_range(uintptr_t addr, size_t size);

static uint64_t now_us(void)
{
    /* Sostituisci col timer di sistema del tuo BSP se ti serve precisione. */
    return (uint64_t)xTaskGetTickCount() * (1000000ULL / configTICK_RATE_HZ);
}

/* xorshift locale: rand() di newlib qui non serve e ci porterebbe dentro
 * inizializzazioni di libc che sul C906 non vogliamo. */
static uint32_t prng(void)
{
    static uint32_t s = 0x2545F491u;

    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void shm_init(void)
{
    memset((void *)g_shm, 0, sizeof(*g_shm));
    __asm__ volatile ("fence rw, rw" ::: "memory");
    flush_dcache_range((uintptr_t)g_shm, sizeof(*g_shm));
}

static void publish(int32_t temp_mC, uint32_t vib_rms, uint32_t drops)
{
    /* 1. payload */
    g_shm->temp_mC = temp_mC;
    g_shm->vib_rms = vib_rms;
    g_shm->ts_us   = now_us();
    g_shm->drops   = drops;

    /* 2. barriera: il payload deve essere visibile PRIMA di seq */
    __asm__ volatile ("fence w, w" ::: "memory");

    /* 3. commit: seq e' l'ultimo store, e' cio' che il reader osserva */
    g_shm->seq++;
    g_shm->magic = MSG_MAGIC;

    __asm__ volatile ("fence rw, rw" ::: "memory");

    /*
     * La regione NON e' mappata uncached lato RTOS: senza questo clean della
     * D-cache Linux legge dati stantii in modo intermittente. E' il bug numero
     * uno di questa integrazione. Il flush copre l'intera struct, quindi anche
     * il payload scritto prima della barriera.
     */
    flush_dcache_range((uintptr_t)g_shm, sizeof(*g_shm));
}

/*
 * Doorbell verso Linux: ON da v1-irq.
 *
 * Due precondizioni, entrambe soddisfatte da qui in poi:
 *
 *  - la patch 0004 sull'SDK, che rende non fatale un mailbox pieno. L'RTOS parte
 *    secondi prima di Linux e a 10 Hz gli 8 slot (MAILBOX_MAX_NUM) si riempiono
 *    in 800 ms mentre nessuno li drena. Senza quella patch l'esaurimento degli
 *    slot fa uscire prvCmdQuRunTask da una funzione task void, e il port RISC-V
 *    salta a 0: RTOS giu', canale congelato;
 *  - il modulo duos_ipc_irq lato Linux, che occupa lo slot handler di IP_SYSTEM
 *    e sveglia il reader. Senza, ogni doorbell finisce in un pr_err dell'ISR.
 *
 * Rimettila a 0 per tornare al comportamento di v0-polling.
 */
#define IPC_DOORBELL 1

#if IPC_DOORBELL
/*
 * Nell'SDK v2 non esiste piu' una request_send_to_cpu(): il percorso verso
 * Linux e' la coda E_QUEUE_CMDQU. prvCmdQuRunTask() prende il messaggio, non
 * riconosce il cmd_id e cade nel ramo default, che e' esattamente quello che
 * scrive il cmdqu_t nel buffer del mailbox e alza l'interrupt.
 *
 * cmd_id e' un bitfield a 7 bit: CMD_SENSOR_READY deve stare sotto 128.
 */
static void doorbell(void)
{
    QueueHandle_t q = main_GetMODHandle(E_QUEUE_CMDQU);
    cmdqu_t tx;

    if (q == NULL)
        return;

    memset(&tx, 0, sizeof(tx));
    tx.ip_id       = IP_SYSTEM;
    tx.cmd_id      = CMD_SENSOR_READY;
    tx.block       = 0;                /* fire & forget, non aspettiamo ack  */
    tx.resv.mstime = 0;                /* 0 = non bloccante                  */
    tx.param_ptr   = SHM_PHYS_ADDR;    /* solo 32 bit utili nel cmdqu        */

    /* timeout 0: se la coda e' piena preferiamo perdere la notifica che
     * bloccare il producer. Il reader in polling recupera comunque. */
    xQueueSend(q, &tx, 0U);
}
#endif /* IPC_DOORBELL */

static void sensor_task(void *arg)
{
    uint32_t n = 0;

    (void)arg;
    shm_init();

    for (;;) {
        int32_t  t   = 40000 + (int32_t)(prng() % 5000);
        uint32_t vib = 900 + (prng() % 400);

        publish(t, vib, 0);
#if IPC_DOORBELL
        doorbell();
#endif

        n++;
#if IPC_RTOS_LOG && IPC_LOG_EVERY
        if ((n % IPC_LOG_EVERY) == 0)
            printf("ipc: seq=%u\n", (unsigned int)n);
#else
        (void)n;
#endif

        vTaskDelay(pdMS_TO_TICKS(100));   /* 10 Hz */
    }
}

void ipc_sensor_task_create(void)
{
#if IPC_RTOS_LOG
    /*
     * Chiamata prima dello scheduler, quando dump_uart_init() ha gia' girato
     * (avviene in post_system_init(), prima di main_cvirtos()). Se
     * l'allocazione del buffer di dump fosse fallita, dump_uart sarebbe NULL e
     * questa dereferenzierebbe zero - ma in quel caso non vedresti nemmeno la
     * riga "log will not print" al boot, che invece c'e'.
     */
    dump_uart_disable();
    printf("ipc: task avviato, shm=0x%08x, doorbell=%s\n",
           (unsigned int)SHM_PHYS_ADDR, IPC_DOORBELL ? "on" : "off");
#endif

    xTaskCreate(sensor_task, "sensor", configMINIMAL_STACK_SIZE * 2, NULL,
                tskIDLE_PRIORITY + 2, NULL);
}
