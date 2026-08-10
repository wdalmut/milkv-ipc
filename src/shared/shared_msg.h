/*
 * shared_msg.h - contratto IPC fra il core FreeRTOS (C906 small) e Linux
 * sul SG2000 / Milk-V Duo S.
 *
 * UNICA FONTE DI VERITA': incluso sia dal task FreeRTOS sia dal reader Linux.
 * Se cambi la struct o SHM_PHYS_ADDR, ricompila ENTRAMBI i lati e riverifica
 * il reserved-memory nel device tree.
 */
#ifndef SHARED_MSG_H
#define SHARED_MSG_H

#include <stdint.h>

/*
 * Indirizzo fisico della finestra condivisa.
 * Deve cadere dentro la regione DRAM sottratta a Linux (vedi
 * board/duos/patches/linux/reserved-memory.dtsi.patch).
 * Allineato a 4 KiB perche' lato Linux lo mappiamo con mmap().
 */
#define SHM_PHYS_ADDR   0x8FE00000u
#define SHM_SIZE        4096u

/* Riconoscimento del payload: finche' non e' scritto, Linux non si fida. */
#define MSG_MAGIC       0xC0FFEE01u

/* cmd_id custom sul mailbox (doorbell). Tieniti fuori dai range dell'SDK. */
#define CMD_SENSOR_READY  0x40u

/*
 * Regole per restare ABI-compatibili fra i due compilatori:
 *  - solo tipi a larghezza fissa (niente int, long, size_t)
 *  - niente puntatori (spazi di indirizzamento diversi)
 *  - niente enum ne' bitfield (layout implementation-defined)
 *  - niente float finche' non hai verificato l'ABI FP di entrambi i lati
 */
typedef struct {
    uint32_t magic;      /* MSG_MAGIC quando il blocco e' valido        */
    uint32_t seq;        /* incrementato dal producer DOPO il payload   */
    int32_t  temp_mC;    /* temperatura in milli-gradi Celsius          */
    uint32_t vib_rms;    /* RMS vibrazione, unita' ADC                  */
    uint64_t ts_us;      /* timestamp del producer, microsecondi        */
    uint32_t drops;      /* campioni persi lato RTOS (diagnostica)      */
    uint32_t _pad;       /* padding esplicito -> sizeof == 32           */
} __attribute__((packed, aligned(8))) sensor_msg_t;

#endif /* SHARED_MSG_H */
