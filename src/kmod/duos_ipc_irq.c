// SPDX-License-Identifier: GPL-2.0
/*
 * duos_ipc_irq.c - porta il doorbell del core FreeRTOS fino a userspace.
 *
 * Il payload NON passa da qui: sta in memoria condivisa e il reader lo legge
 * con mmap su /dev/mem. Questo modulo trasporta solo il campanello.
 *
 * Perche' serve un modulo. L'ISR di cvi_rtos_cmdqu smista i messaggi in arrivo
 * dall'RTOS in tre modi: se block==1 sveglia chi attende in RTOS_CMDQU_SEND_WAIT;
 * altrimenti, se c'e' un handler registrato per quell'ip_id, lo chiama in
 * contesto interrupt; altrimenti stampa "error ip=.. cmd=..". Userspace non ha
 * modo di bloccarsi su un messaggio non sollecitato - SEND_WAIT e'
 * manda-e-attendi-risposta - quindi l'unico gancio per un push e' l'handler
 * in-kernel, che e' quello che registriamo qui.
 *
 * Modello: un contatore monotono, non una coda di eventi. La finestra condivisa
 * e' un singolo slot che il producer sovrascrive, quindi una coda di doorbell
 * arretrate punterebbe tutta allo stesso dato. Il reader legge il contatore e
 * ricava da se' quante notifiche ha perso.
 *
 * Dipendenza: cv181x_rtos_cmdqu.ko deve essere gia' caricato, esporta
 * request_rtos_irq()/free_rtos_irq().
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "rtos_cmdqu.h"		/* IP_SYSTEM, request_rtos_irq()   */
#include "shared_msg.h"		/* CMD_SENSOR_READY                */

#define DEV_NAME "duos-ipc"

static DECLARE_WAIT_QUEUE_HEAD(bell_wq);

/* Contatore monotono di doorbell. Wrappa a 2^32 e va bene: il reader fa solo
 * confronti di disuguaglianza e sottrazioni, entrambe corrette al wrap. */
static atomic_t bell = ATOMIC_INIT(0);

/* Doorbell con un cmd_id che non ci aspettavamo: diagnostica, non un errore.
 * Contato invece che stampato, perche' l'handler gira in contesto interrupt. */
static atomic_t unexpected = ATOMIC_INIT(0);

/* Stato per-fd: due reader non si rubano i wakeup a vicenda. */
struct bell_reader {
	unsigned int last;
};

/*
 * Chiamata dall'ISR di cvi_rtos_cmdqu, in contesto hard-IRQ e con lo spinlock
 * del mailbox preso. Qui dentro non si dorme e non si stampa.
 */
static void duos_ipc_doorbell(unsigned int cmd_id, unsigned int param_ptr,
			      void *dev_id)
{
	(void)param_ptr;	/* l'indirizzo lo conosce gia' il reader */
	(void)dev_id;

	if (cmd_id != CMD_SENSOR_READY) {
		atomic_inc(&unexpected);
		return;
	}

	atomic_inc(&bell);
	wake_up_interruptible(&bell_wq);
}

static int duos_ipc_open(struct inode *inode, struct file *filp)
{
	struct bell_reader *r = kzalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return -ENOMEM;

	/*
	 * Si parte dal valore corrente, non da zero: il reader non prende un
	 * wakeup spurio all'apertura e non prova a recuperare campioni che nel
	 * singolo slot condiviso non esistono piu'.
	 */
	r->last = (unsigned int)atomic_read(&bell);
	filp->private_data = r;

	return 0;
}

static int duos_ipc_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	filp->private_data = NULL;

	return 0;
}

static ssize_t duos_ipc_read(struct file *filp, char __user *buf, size_t len,
			     loff_t *off)
{
	struct bell_reader *r = filp->private_data;
	u32 now;

	if (len < sizeof(u32))
		return -EINVAL;

	if ((unsigned int)atomic_read(&bell) == r->last) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(bell_wq,
			(unsigned int)atomic_read(&bell) != r->last))
			return -ERESTARTSYS;
	}

	now = (u32)atomic_read(&bell);
	r->last = now;

	if (copy_to_user(buf, &now, sizeof(now)))
		return -EFAULT;

	return sizeof(now);
}

static __poll_t duos_ipc_poll(struct file *filp, poll_table *wait)
{
	struct bell_reader *r = filp->private_data;

	poll_wait(filp, &bell_wq, wait);

	if ((unsigned int)atomic_read(&bell) != r->last)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations duos_ipc_fops = {
	/*
	 * .owner fa si' che un fd aperto tenga un riferimento al modulo: un
	 * rmmod con un reader in attesa viene rifiutato con EBUSY invece di
	 * lasciare l'handler a scrivere su strutture liberate.
	 */
	.owner		= THIS_MODULE,
	.open		= duos_ipc_open,
	.release	= duos_ipc_release,
	.read		= duos_ipc_read,
	.poll		= duos_ipc_poll,
	.llseek		= no_llseek,
};

static struct miscdevice duos_ipc_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= DEV_NAME,
	.fops	= &duos_ipc_fops,
	.mode	= 0440,
};

static int __init duos_ipc_init(void)
{
	int ret;

	ret = misc_register(&duos_ipc_misc);
	if (ret) {
		pr_err("duos-ipc: misc_register fallita: %d\n", ret);
		return ret;
	}

	/*
	 * Registrato per ultimo: da qui in poi l'handler puo' partire, e deve
	 * trovare il device gia' in piedi.
	 *
	 * rtos_irqaction ha un solo slot per ip_id, quindi finche' siamo
	 * caricati possediamo IP_SYSTEM. Una RTOS_CMDQU_REQUEST da userspace
	 * sullo stesso ip ci scalzerebbe senza dircelo.
	 */
	ret = request_rtos_irq(IP_SYSTEM, duos_ipc_doorbell, DEV_NAME, NULL);
	if (ret) {
		pr_err("duos-ipc: request_rtos_irq(IP_SYSTEM) fallita: %d\n", ret);
		misc_deregister(&duos_ipc_misc);
		return ret;
	}

	pr_info("duos-ipc: /dev/%s pronto, doorbell cmd_id=0x%x su IP_SYSTEM\n",
		DEV_NAME, CMD_SENSOR_READY);

	return 0;
}

static void __exit duos_ipc_exit(void)
{
	/* Prima si spegne la sorgente, poi si toglie il collettore. */
	free_rtos_irq(IP_SYSTEM);
	misc_deregister(&duos_ipc_misc);

	pr_info("duos-ipc: scaricato, %u doorbell, %u con cmd_id inatteso\n",
		(unsigned int)atomic_read(&bell),
		(unsigned int)atomic_read(&unexpected));
}

module_init(duos_ipc_init);
module_exit(duos_ipc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("duos-ipc");
MODULE_DESCRIPTION("Doorbell FreeRTOS -> userspace per il canale IPC del SG2000");
