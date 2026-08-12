// SPDX-License-Identifier: GPL-2.0
/*
 * duos_ipc_irq.c - canale IPC dal core FreeRTOS a userspace.
 *
 * Espone /dev/duos-ipc: una read() blocca finche' il core C906 non pubblica un
 * campione nuovo, poi restituisce sizeof(sensor_msg_t) byte. Il campanello
 * arriva dal mailbox, il payload dalla finestra di memoria condivisa: il
 * modulo unisce le due cose e userspace ne vede una sola.
 *
 * Perche' esiste. L'ISR di cvi_rtos_cmdqu smista i messaggi in arrivo dall'RTOS
 * in tre modi: con block == 1 sveglia chi attende in RTOS_CMDQU_SEND_WAIT;
 * altrimenti chiama l'handler registrato per quell'ip_id; altrimenti stampa
 * "error ip=.. cmd=..". Userspace non ha modo di bloccarsi su un messaggio non
 * sollecitato - SEND_WAIT e' manda-e-attendi-risposta - quindi l'unico gancio
 * per un push e' l'handler in-kernel, che e' quello che registriamo qui.
 *
 * Perche' il payload passa da qui e non da /dev/mem. Leggere la finestra da
 * userspace richiede root e un kernel senza CONFIG_STRICT_DEVMEM, cioe' la
 * capacita' di leggere TUTTA la memoria fisica per avere 4 KiB. In piu' il
 * protocollo seqlock andrebbe reimplementato da ogni consumatore, in ogni
 * linguaggio, e sbagliarlo e' silenzioso: ottieni dati plausibili e strappati.
 * Qui e' scritto una volta.
 *
 * Dipendenza: cv181x_rtos_cmdqu.ko deve essere gia' caricato, esporta
 * request_rtos_irq()/free_rtos_irq().
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "rtos_cmdqu.h"		/* IP_SYSTEM, request_rtos_irq()   */
#include "shared_msg.h"		/* sensor_msg_t, CMD_SENSOR_READY  */

#define DEV_NAME	"duos-ipc"

/* Quante volte ritentare uno snapshot strappato prima di arrendersi. A 10 Hz
 * non deve servire nemmeno il secondo giro: se serve il quinto, il producer sta
 * scrivendo molto piu' in fretta di quanto crediamo. */
#define SNAPSHOT_TRIES	5

struct duos_ipc {
	void __iomem		*shm;
	resource_size_t		shm_size;
	struct miscdevice	misc;
	wait_queue_head_t	wq;
	atomic_t		bell;		/* doorbell ricevute, monotono */
	atomic_t		unexpected;	/* doorbell con cmd_id ignoto  */
	struct mutex		tx_lock;	/* serializza le write()       */
	u32			cmd_seq;	/* seq della finestra in uscita */
};

/*
 * L'handler di cvi_rtos_cmdqu non porta un contesto nostro: si registra un
 * puntatore a funzione e basta. Con un solo device di questo tipo, un singolo
 * puntatore globale e' la cosa piu' onesta.
 */
static struct duos_ipc *g_ipc;

/* Stato per-fd: due reader non si rubano i wakeup a vicenda. */
struct bell_reader {
	unsigned int last;
};

/*
 * Chiamata dall'ISR di cvi_rtos_cmdqu, in contesto hard-IRQ e con lo spinlock
 * del mailbox preso. Qui dentro non si dorme, non si allocca e non si stampa.
 */
static void duos_ipc_doorbell(unsigned int cmd_id, unsigned int param_ptr,
			      void *dev_id)
{
	struct duos_ipc *ipc = g_ipc;

	(void)param_ptr;	/* l'indirizzo lo conosce gia' il driver */
	(void)dev_id;

	if (!ipc)
		return;

	if (cmd_id != CMD_SENSOR_READY) {
		atomic_inc(&ipc->unexpected);
		return;
	}

	atomic_inc(&ipc->bell);
	wake_up_interruptible(&ipc->wq);
}

/*
 * Il seqlock del produttore, letto dal lato consumatore.
 *
 * Il C906 scrive il payload, mette una barriera, poi incrementa seq: seq e' il
 * commit. Qui si legge seq, si copia, si rilegge seq: se e' cambiato la copia e'
 * strappata e va buttata. Questo e' l'unico posto del sistema che deve conoscere
 * il protocollo.
 */
static int duos_ipc_snapshot(struct duos_ipc *ipc, sensor_msg_t *out)
{
	int i;

	for (i = 0; i < SNAPSHOT_TRIES; i++) {
		u32 seq0, seq1;

		seq0 = ioread32(ipc->shm + offsetof(sensor_msg_t, seq));

		memcpy_fromio(out, ipc->shm, sizeof(*out));
		rmb();

		seq1 = ioread32(ipc->shm + offsetof(sensor_msg_t, seq));
		if (seq0 != seq1)
			continue;

		/* Finche' il producer non ha scritto il magic, la finestra
		 * contiene quello che ci ha lasciato il boot precedente. */
		if (out->magic != MSG_MAGIC)
			return -ENODATA;

		return 0;
	}

	return -EAGAIN;
}

static int duos_ipc_open(struct inode *inode, struct file *filp)
{
	struct bell_reader *r = kzalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return -ENOMEM;

	/*
	 * Si parte dal valore corrente, non da zero: il lettore non prende un
	 * wakeup spurio all'apertura e non prova a recuperare campioni che nel
	 * singolo slot condiviso non esistono piu'.
	 */
	r->last = (unsigned int)atomic_read(&g_ipc->bell);
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
	struct duos_ipc *ipc = g_ipc;
	sensor_msg_t snap;
	int ret;

	if (len < sizeof(snap))
		return -EINVAL;

	if ((unsigned int)atomic_read(&ipc->bell) == r->last) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(ipc->wq,
			(unsigned int)atomic_read(&ipc->bell) != r->last))
			return -ERESTARTSYS;
	}

	r->last = (unsigned int)atomic_read(&ipc->bell);

	ret = duos_ipc_snapshot(ipc, &snap);
	if (ret)
		return ret;

	if (copy_to_user(buf, &snap, sizeof(snap)))
		return -EFAULT;

	return sizeof(snap);
}

/*
 * Verso opposto: un comando dall'host al micro.
 *
 * Stesso protocollo dell'altro senso, a ruoli invertiti: si scrive il payload,
 * barriera, poi si incrementa seq - che e' il commit. Poi si suona la doorbell
 * con rtos_cmdqu_send(), esportata da cv181x_rtos_cmdqu.
 *
 * magic e seq li mette il driver, non userspace: cosi' un programma non puo'
 * pubblicare un blocco con un magic sbagliato ne' far arretrare seq, che dal
 * lato micro sembrerebbe un comando mai arrivato.
 *
 * Il mutex serializza fra piu' scrittori. Non protegge dal micro, che questa
 * regione non la scrive mai - ed e' proprio quella garanzia che gli permette di
 * invalidare la cache senza perdere nulla.
 */
static ssize_t duos_ipc_write(struct file *filp, const char __user *buf,
			      size_t len, loff_t *off)
{
	struct duos_ipc *ipc = g_ipc;
	void __iomem *win = ipc->shm + CMD_SHM_OFFSET;
	host_cmd_t cmd;
	cmdqu_t bell;
	int ret;

	if (len != sizeof(cmd))
		return -EINVAL;

	if (CMD_SHM_OFFSET + sizeof(cmd) > ipc->shm_size)
		return -ENOSPC;

	if (copy_from_user(&cmd, buf, sizeof(cmd)))
		return -EFAULT;

	mutex_lock(&ipc->tx_lock);

	iowrite32(CMD_MAGIC, win + offsetof(host_cmd_t, magic));
	iowrite32(cmd.cmd, win + offsetof(host_cmd_t, cmd));
	iowrite32(cmd.arg, win + offsetof(host_cmd_t, arg));
	memcpy_toio(win + offsetof(host_cmd_t, data), cmd.data, CMD_DATA_LEN);

	/* Il payload deve essere in DRAM prima che seq lo dichiari valido. */
	wmb();
	iowrite32(++ipc->cmd_seq, win + offsetof(host_cmd_t, seq));
	wmb();

	memset(&bell, 0, sizeof(bell));
	bell.ip_id = IP_SYSTEM;
	bell.cmd_id = CMD_HOST_READY;
	bell.block = 0;
	bell.param_ptr = 0;

	ret = rtos_cmdqu_send(&bell);
	mutex_unlock(&ipc->tx_lock);

	if (ret) {
		/* Il payload e' in finestra ma il micro non lo sa. Non e'
		 * recuperabile da qui: chi scrive decide se ritentare. */
		dev_warn_ratelimited(ipc->misc.this_device,
				     "doorbell verso il micro fallita: %d\n", ret);
		return ret;
	}

	return sizeof(cmd);
}

static __poll_t duos_ipc_poll(struct file *filp, poll_table *wait)
{
	struct bell_reader *r = filp->private_data;
	struct duos_ipc *ipc = g_ipc;

	poll_wait(filp, &ipc->wq, wait);

	if ((unsigned int)atomic_read(&ipc->bell) != r->last)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations duos_ipc_fops = {
	/*
	 * .owner fa si' che un fd aperto tenga un riferimento al modulo: un
	 * rmmod con un lettore in attesa viene rifiutato con EBUSY invece di
	 * lasciare l'handler a scrivere su strutture liberate.
	 */
	.owner		= THIS_MODULE,
	.open		= duos_ipc_open,
	.release	= duos_ipc_release,
	.read		= duos_ipc_read,
	.write		= duos_ipc_write,
	.poll		= duos_ipc_poll,
	.llseek		= no_llseek,
};

/*
 * Il contatore delle doorbell non entra nel formato di read(), che resta la
 * sola struct: chi vuole distinguere "campioni sovrascritti" da "notifiche
 * accorpate" lo legge da qui.
 */
static ssize_t bell_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%u\n", (unsigned int)atomic_read(&g_ipc->bell));
}
static DEVICE_ATTR_RO(bell);

static ssize_t unexpected_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  (unsigned int)atomic_read(&g_ipc->unexpected));
}
static DEVICE_ATTR_RO(unexpected);

static struct attribute *duos_ipc_attrs[] = {
	&dev_attr_bell.attr,
	&dev_attr_unexpected.attr,
	NULL,
};
ATTRIBUTE_GROUPS(duos_ipc);

static int duos_ipc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *mem_np;
	struct resource res;
	struct duos_ipc *ipc;
	int ret;

	if (g_ipc)
		return -EBUSY;	/* un solo canale, l'handler e' uno solo */

	ipc = devm_kzalloc(dev, sizeof(*ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	/*
	 * L'indirizzo della finestra sta nel device tree, non nel codice: e' lo
	 * stesso nodo che la sottrae a Linux, quindi mappatura e riserva non
	 * possono divergere. Un disallineamento fra le due non darebbe un
	 * errore, darebbe dati sbagliati.
	 */
	mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_np) {
		dev_err(dev, "manca la proprieta' memory-region nel device tree\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret) {
		dev_err(dev, "memory-region senza reg valida: %d\n", ret);
		return ret;
	}

	if (resource_size(&res) < sizeof(sensor_msg_t)) {
		dev_err(dev, "finestra da %llu byte, ne servono %zu\n",
			(unsigned long long)resource_size(&res),
			sizeof(sensor_msg_t));
		return -EINVAL;
	}

	/*
	 * ioremap e non memremap: i due core non sono cache-coherent e serve
	 * una mappatura non cacheable. E' la stessa cosa che si otteneva con
	 * /dev/mem + O_SYNC, che usa pgprot_noncached(). Con una mappatura
	 * cacheable si leggono valori stantii in modo intermittente.
	 */
	ipc->shm = devm_ioremap(dev, res.start, resource_size(&res));
	if (!ipc->shm) {
		dev_err(dev, "ioremap di %pR fallita\n", &res);
		return -ENOMEM;
	}
	ipc->shm_size = resource_size(&res);

	init_waitqueue_head(&ipc->wq);
	mutex_init(&ipc->tx_lock);
	atomic_set(&ipc->bell, 0);
	atomic_set(&ipc->unexpected, 0);

	ipc->misc.minor = MISC_DYNAMIC_MINOR;
	ipc->misc.name = DEV_NAME;
	ipc->misc.fops = &duos_ipc_fops;
	/*
	 * 0444: leggere i dati del sensore non richiede privilegi, ed e' cio'
	 * che rende vera l'affermazione "non serve root". Scrivere comandi al
	 * micro invece resta a root, che scavalca i permessi: e' asimmetrico di
	 * proposito, perche' le due operazioni non hanno lo stesso peso.
	 */
	ipc->misc.mode = 0444;
	ipc->misc.groups = duos_ipc_groups;

	ret = misc_register(&ipc->misc);
	if (ret) {
		dev_err(dev, "misc_register fallita: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, ipc);
	g_ipc = ipc;

	/*
	 * Registrato per ultimo: da qui in poi l'handler puo' partire, e deve
	 * trovare tutto il resto gia' in piedi.
	 *
	 * rtos_irqaction ha un solo slot per ip_id, quindi finche' siamo
	 * caricati possediamo IP_SYSTEM. Una RTOS_CMDQU_REQUEST da userspace
	 * sullo stesso ip ci scalzerebbe senza dircelo.
	 */
	ret = request_rtos_irq(IP_SYSTEM, duos_ipc_doorbell, DEV_NAME, NULL);
	if (ret) {
		dev_err(dev, "request_rtos_irq(IP_SYSTEM) fallita: %d\n", ret);
		g_ipc = NULL;
		misc_deregister(&ipc->misc);
		return ret;
	}

	dev_info(dev, "/dev/%s pronto: finestra %pR, doorbell cmd_id=0x%x\n",
		 DEV_NAME, &res, CMD_SENSOR_READY);

	return 0;
}

static int duos_ipc_remove(struct platform_device *pdev)
{
	struct duos_ipc *ipc = platform_get_drvdata(pdev);

	/* Prima si spegne la sorgente, poi si toglie il collettore. */
	free_rtos_irq(IP_SYSTEM);
	g_ipc = NULL;
	misc_deregister(&ipc->misc);

	dev_info(&pdev->dev, "scaricato: %u doorbell, %u con cmd_id inatteso\n",
		 (unsigned int)atomic_read(&ipc->bell),
		 (unsigned int)atomic_read(&ipc->unexpected));

	return 0;
}

static const struct of_device_id duos_ipc_of_match[] = {
	{ .compatible = "corley,duos-ipc" },
	{ }
};
MODULE_DEVICE_TABLE(of, duos_ipc_of_match);

static struct platform_driver duos_ipc_driver = {
	.probe	= duos_ipc_probe,
	.remove	= duos_ipc_remove,
	.driver	= {
		.name		= DEV_NAME,
		.of_match_table	= duos_ipc_of_match,
	},
};

module_platform_driver(duos_ipc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("duos-ipc");
MODULE_DESCRIPTION("Canale IPC FreeRTOS -> userspace per il SG2000");
