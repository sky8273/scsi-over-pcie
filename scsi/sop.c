/*
 *    SCSI over PCI (SoP) driver
 *    Copyright 2012 Hewlett-Packard Development Company, L.P.
 *    Copyright 2012 SanDisk Inc.
 *
 *    This program is licensed under the GNU General Public License
 *    version 2
 *
 *    This program is distributed "as is" and WITHOUT ANY WARRANTY
 *    of any kind whatsoever, including without limitation the implied
 *    warranty of MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *    Please see the GNU General Public License v.2 at
 *    http://www.gnu.org/licenses/licenses.en.html for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 *    Questions/Comments/Bugfixes to iss_storagedev@hp.com
 *
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/sched.h>
/* #include <asm/byteorder.h> */
#include <linux/init.h>
#include <linux/version.h>
#include <linux/completion.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "sop_kernel_compat.h"
#include "sop.h"

#define DRIVER_VERSION "1.0.0"
#define DRIVER_NAME "sop (v " DRIVER_VERSION ")"
#define SOP "sop"

MODULE_AUTHOR("Hewlett-Packard Company");
MODULE_AUTHOR("SanDisk Inc.");
MODULE_DESCRIPTION("sop driver" DRIVER_VERSION);
MODULE_SUPPORTED_DEVICE("sop devices");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#ifndef PCI_VENDOR_SANDISK
#define PCI_VENDOR_SANDISK 0x15b7
#endif

DEFINE_PCI_DEVICE_TABLE(sop_id_table) = {
	{ PCI_VENDOR_SANDISK, 0x0012, PCI_VENDOR_SANDISK, 0x0000 },
	{ PCI_VENDOR_SANDISK, 0x0021, PCI_VENDOR_SANDISK, 0x0000 },
	{ PCI_VENDOR_SANDISK, 0x2100, PCI_VENDOR_SANDISK, 0x0000 },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, sop_id_table);

static inline struct sop_device *shost_to_hba(struct Scsi_Host *sh)
{
	unsigned long *priv = shost_priv(sh);
	return (struct sop_device *) *priv;
}

static ssize_t host_show_sopstats(struct device *dev,
        struct device_attribute *attr, char *buf)
{
        struct sop_device *h;
        struct Scsi_Host *shost = class_to_shost(dev);
	int curr;

        h = shost_to_hba(shost);
	curr = atomic_read(&h->curr_outstanding_commands);
        return snprintf(buf, 40, "max out: %d curr out: %d\n", h->max_outstanding_commands, curr);
}

static DEVICE_ATTR(sopstats, S_IRUGO, host_show_sopstats, NULL);

static struct device_attribute *sop_host_attrs[] = {
        &dev_attr_sopstats,
        NULL,
};

static int controller_num;

static int sop_queuecommand(struct Scsi_Host *h, struct scsi_cmnd *sc);
static int sop_change_queue_depth(struct scsi_device *sdev,
        int qdepth, int reason);
static int sop_abort_handler(struct scsi_cmnd *sc);
static int sop_device_reset_handler(struct scsi_cmnd *sc);
static int sop_slave_alloc(struct scsi_device *sdev);
static void sop_slave_destroy(struct scsi_device *sdev);
static int sop_compat_ioctl(struct scsi_device *dev, int cmd, void *arg);
static int sop_ioctl(struct scsi_device *dev, int cmd, void *arg);

static struct scsi_host_template sop_template = {
	.module				= THIS_MODULE,
	.name				= DRIVER_NAME,
	.proc_name			= DRIVER_NAME,
	.queuecommand			= sop_queuecommand,
	.change_queue_depth		= sop_change_queue_depth,
	.this_id			= -1,
	.use_clustering			= ENABLE_CLUSTERING,
	.eh_abort_handler		= sop_abort_handler,
	.eh_device_reset_handler	= sop_device_reset_handler,
	.ioctl				= sop_ioctl,
	.slave_alloc			= sop_slave_alloc,
	.slave_destroy			= sop_slave_destroy,
#ifdef CONFIG_COMPAT
	.compat_ioctl			= sop_compat_ioctl,
#endif
#if 0
	.sdev_attrs			= sop_sdev_attrs,
#endif
	.shost_attrs			= sop_host_attrs,
	.max_sectors			= (MAX_SGLS * 8),
};

static pci_ers_result_t sop_pci_error_detected(struct pci_dev *dev,
				enum pci_channel_state error);
static pci_ers_result_t sop_pci_mmio_enabled(struct pci_dev *dev);
static pci_ers_result_t sop_pci_link_reset(struct pci_dev *dev);
static pci_ers_result_t sop_pci_slot_reset(struct pci_dev *dev);
static void sop_pci_resume(struct pci_dev *dev);

static struct pci_error_handlers sop_pci_error_handlers = {
	.error_detected = sop_pci_error_detected,
	.mmio_enabled = sop_pci_mmio_enabled,
	.link_reset = sop_pci_link_reset,
	.slot_reset = sop_pci_slot_reset,
	.resume = sop_pci_resume,
};

/* 
 * 32-bit readq and writeq implementations taken from old
 * version of arch/x86/include/asm/io.h 
 */
#ifndef readq
static inline u64 readq(const volatile void __iomem *addr)
{
	const volatile u32 __iomem *p = addr;
	u32 low, high;

	low = readl(p);
	high = readl(p + 1);

	return low + ((u64)high << 32);
}
#endif

#ifndef writeq
static inline void writeq(u64 val, volatile void __iomem *addr)
{
	writel(val, addr);
	writel(val >> 32, addr+4);
}
#endif

static inline int check_for_read_failure(__iomem void *sig)
{
	/*
	 * do a readl of a known constant value, if it comes back -1,
	 * we know we're not able to read.
	 */
	u64 signature;

	signature = readq(sig);
	return signature == 0xffffffffffffffffULL;
}

static inline int safe_readw(__iomem void *sig, u16 *value,
				const volatile void __iomem *addr)
{
	*value = readw(addr);
	if (unlikely(*value == (u16) 0xffff)) {
		if (check_for_read_failure(sig))
			return -1;
	}
	return 0;
}

static inline int safe_readl(__iomem void *sig, u32 *value,
				const volatile void __iomem *addr)
{
	*value = readl(addr);
	if (unlikely(*value == 0xffffffff)) {
		if (check_for_read_failure(sig))
			return -1;
	}
	return 0;
}

static inline int safe_readq(__iomem void *sig, u64 *value,
				const volatile void __iomem *addr)
{
	*value = readq(addr);
	if (unlikely(*value == 0xffffffffffffffffULL)) {
		if (check_for_read_failure(sig))
			return -1;
	}
	return 0;
}

static void free_q_request_buffers(struct queue_info *q)
{
	if (q->request_bits) {
		kfree(q->request_bits);
		q->request_bits = NULL;
	}
	if (q->request) {
		kfree(q->request);
		q->request = NULL;
	}
}

static int allocate_sgl_area(struct sop_device *h,
		struct queue_info *q)
{
	size_t total_size = q->qdepth * MAX_SGLS *
				sizeof(struct pqi_sgl_descriptor);

	q->sg = pci_alloc_consistent(h->pdev, total_size, &q->sg_bus_addr);
	return (q->sg) ? 0 : -ENOMEM;
}

static void free_sgl_area(struct sop_device *h, struct queue_info *q)
{
	size_t total_size = q->qdepth * MAX_SGLS *
				sizeof(struct pqi_sgl_descriptor);

	if (!q->sg)
		return;
	pci_free_consistent(h->pdev, total_size, q->sg, q->sg_bus_addr);
	q->sg = NULL;
}

static int allocate_q_request_buffers(struct queue_info *q,
	int nbuffers, int buffersize)
{
	q->qdepth = nbuffers;
	q->request_bits = kzalloc((BITS_TO_LONGS(nbuffers) + 1) *
					sizeof(unsigned long), GFP_KERNEL);
	if (!q->request_bits)
		goto bailout;
	q->request = kzalloc(buffersize * nbuffers, GFP_KERNEL);
	if (!q->request)
		goto bailout;
	return 0;

bailout:
	free_q_request_buffers(q);
	return -ENOMEM;
}

static int pqi_device_queue_alloc(struct sop_device *h,
		struct pqi_device_queue **xq,
		u16 n_q_elements, u8 q_element_size_over_16,
		int queue_direction, int queue_pair_index)
{
	void *vaddr = NULL;
	dma_addr_t dhandle;

	int total_size = (n_q_elements * q_element_size_over_16 * 16) +
				sizeof(u64);
#if 0
	int remainder = total_size % 64;

	/* this adjustment we think is unnecessary because we now allocate queues
	 * separately not in a big array, and pci_alloc_consistent returns
	 * page aligned memory.
	 */

	total_size += remainder ? 64 - remainder : 0;
#endif
	*xq = kzalloc(sizeof(**xq), GFP_KERNEL);
	if (!*xq) {
		dev_warn(&h->pdev->dev, "Failed to alloc pqi struct #%d, dir %d\n",
			queue_pair_index, queue_direction);
		goto bailout;
	}
	vaddr = pci_alloc_consistent(h->pdev, total_size, &dhandle);
	if (!vaddr) {
		dev_warn(&h->pdev->dev, "Failed to alloc PCI buffer #%d, dir %d\n",
			queue_pair_index, queue_direction);
		goto bailout;
	}
	(*xq)->dhandle = dhandle;
	(*xq)->vaddr = vaddr;
	(*xq)->vaddr = vaddr;

	if (queue_direction == PQI_DIR_TO_DEVICE) {
#if 0
		dev_warn(&h->pdev->dev, "4 allocating request buffers\n");
		qinfo = &h->qinfo[queue_pair_index];
		if (allocate_q_request_buffers(qinfo, n_q_elements,
				sizeof(struct sop_request)))
			goto bailout;
		dev_warn(&h->pdev->dev, "   6 Allocated #%d\n", queue_pair_index);
		/* Allocate SGL area for submission queue */
		if (allocate_sgl_area(h, qinfo))
			goto bailout;
#endif
	}

	if (queue_direction == PQI_DIR_TO_DEVICE) {
		(*xq)->index.to_dev.ci = vaddr +
			q_element_size_over_16 * 16 * n_q_elements;
		/* producer idx is unknown now, hardware will tell us later */
	} else {
		(*xq)->index.from_dev.pi = vaddr +
			q_element_size_over_16 * 16 * n_q_elements;
		/* consumer idx is unknown now, hardware will tell us later */
	}
	(*xq)->queue_id = qpindex_to_qid(queue_pair_index,
				(queue_direction == PQI_DIR_TO_DEVICE));
	(*xq)->unposted_index = 0;
	(*xq)->element_size = q_element_size_over_16 * 16;
	(*xq)->nelements = n_q_elements;
	return 0;

bailout:
	dev_warn(&h->pdev->dev, "Problem allocing queues\n");
	if (vaddr)
		pci_free_consistent(h->pdev, total_size, vaddr, dhandle);
	if (*xq)
		kfree(*xq);
	*xq = NULL;
	return -ENOMEM;
}

static void pqi_device_queue_init(struct pqi_device_queue *q,
		__iomem u16 *register_index, volatile u16 *volatile_index,
		int direction)
{
	if (direction == PQI_DIR_TO_DEVICE) {
		q->index.to_dev.pi = register_index;
		q->index.to_dev.ci = volatile_index;
	} else {
		q->index.from_dev.pi = volatile_index;
		q->index.from_dev.ci = register_index;
	}
	q->unposted_index = 0;
	*volatile_index = 0;
	spin_lock_init(&q->qlock);
	spin_lock_init(&q->index_lock);
}

static void pqi_device_queue_free(struct sop_device *h, struct pqi_device_queue *q)
{
	size_t total_size, n_q_elements, element_size;

	if (q == NULL)
		return;

	n_q_elements = q->nelements;
	element_size = q->element_size;
	total_size = n_q_elements * element_size + sizeof(u64);
	pci_free_consistent(h->pdev, total_size, q->vaddr, q->dhandle);

	kfree(q);
}

static void pqi_iq_buffer_free(struct sop_device *h, struct queue_info *qinfo)
{
	free_q_request_buffers(qinfo);
	free_sgl_area(h, qinfo);
}

static int pqi_iq_data_alloc(struct sop_device *h, struct queue_info *qinfo)
{
	int queue_pair_index = qinfo_to_qid(qinfo);
	int n_q_elements = qinfo->iq->nelements;

	if (allocate_q_request_buffers(qinfo, n_q_elements,
		sizeof(struct sop_request))) {
		dev_warn(&h->pdev->dev, "Failed to alloc rq buffers #%d\n", 
			queue_pair_index);
		goto bailout_iq;
	}

	/* Allocate SGL area for submission queue */
	if (allocate_sgl_area(h, qinfo)) {
		dev_warn(&h->pdev->dev, "Failed to alloc SGL #%d\n",
			queue_pair_index);
		goto bailout_iq;
	}
	return 0;

bailout_iq:
	pqi_iq_buffer_free(h, qinfo);
	return -ENOMEM;
}

static int pqi_to_device_queue_is_full(struct pqi_device_queue *q,
				int nelements)
{
	u16 qci;
	u32 nfree;

	if (safe_readw(&q->registers->signature, &qci, q->index.to_dev.ci))
		return 0;
	qci = le16_to_cpu(*(u16 *) &qci);

	if (q->unposted_index > qci)
		nfree = q->nelements - q->unposted_index + qci - 1;
	else if (q->unposted_index < qci)
		nfree = qci - q->unposted_index - 1;
	else
		nfree = q->nelements;
	return (nfree < nelements);
}

static int pqi_from_device_queue_is_empty(struct pqi_device_queue *q)
{
	u16 qpi;

	if (safe_readw(&q->registers->signature, &qpi, q->index.from_dev.pi))
		return 0;
	qpi = le16_to_cpu(qpi);
	return qpi == q->unposted_index;
}

static void *pqi_alloc_elements(struct pqi_device_queue *q,
					int nelements)
{
	void *p;

	if (pqi_to_device_queue_is_full(q, nelements)) {
		printk(KERN_WARNING "pqi device queue %d is full!\n", q->queue_id);
		return ERR_PTR(-ENOMEM);
	}

	/* If the requested number of elements would wrap around the
	 * end of the ring buffer, insert NULL IUs to the end of the
	 * ring buffer.  This simplifies the code which has to fill
	 * in the IUs as it doesn't have to deal with wrapping
	 */
	if (q->nelements - q->unposted_index < nelements) {
		int extra_elements = q->nelements - q->unposted_index;
		if (pqi_to_device_queue_is_full(q, nelements + extra_elements)) {
			printk(KERN_WARNING "pqi_alloc_elements, device queue is full!\n");
			printk(KERN_WARNING "q->nelements = %d, q->unposted_index = %hu, extra_elements = %d\n",
					q->nelements, q->unposted_index, extra_elements);
			return ERR_PTR(-ENOMEM);
		}
		p = q->vaddr + q->unposted_index * q->element_size;
		memset(p, 0, (q->nelements - q->unposted_index) *
						q->element_size);
		q->unposted_index = 0;
	}
	p = q->vaddr + q->unposted_index * q->element_size;
	q->unposted_index = (q->unposted_index + nelements) % q->nelements;
	return p;
}

static int pqi_dequeue_from_device(struct pqi_device_queue *q, void *element)
{
	void *p;

	if (pqi_from_device_queue_is_empty(q))
		return PQI_QUEUE_EMPTY;

	p = q->vaddr + q->unposted_index * q->element_size;
	/* printk(KERN_WARNING "DQ: p = %p, q->unposted_index = %hu, n = %hu\n",
				p, q->unposted_index, q->nelements); */
	memcpy(element, p, q->element_size);
	q->unposted_index = (q->unposted_index + 1) % q->nelements;
	/* printk(KERN_WARNING "After DQ: q->unposted_index = %hu\n",
				q->unposted_index); */
	return 0;
}

static u8 pqi_peek_ui_type_from_device(struct pqi_device_queue *q)
{
	u8 *p;

	p = q->vaddr + q->unposted_index * q->element_size;
	return *p;
}
static u16 pqi_peek_request_id_from_device(struct pqi_device_queue *q)
{
	u8 *p;

	p = q->vaddr + q->unposted_index * q->element_size + 8;
	return *(u16 *) p;
}

static int xmargin=8;
static int amargin=60;

static void print_bytes(unsigned char *c, int len, int hex, int ascii)
{

	int i;
	unsigned char *x;

	if (hex) {
		x = c;
		for (i = 0; i < len; i++) {
			if ((i % xmargin) == 0 && i > 0)
				pr_warn("\n");
			if ((i % xmargin) == 0)
				pr_warn("0x%04x:", i);
			pr_warn(" %02x", *x);
			x++;
		}
		pr_warn("\n");
	}
	if (ascii) {
		x = c;
		for (i = 0; i < len; i++) {
			if ((i % amargin) == 0 && i > 0)
				pr_warn("\n");
			if ((i % amargin) == 0)
				pr_warn("0x%04x:", i);
			if (*x > 26 && *x < 128)
				pr_warn("%c", *x);
			else
				pr_warn(".");
			x++;
		}
		pr_warn("\n");
	}
}

static void print_iu(unsigned char *iu)
{
	u16 iu_length;

	memcpy(&iu_length, &iu[2], 2);
	iu_length = le16_to_cpu(iu_length) + 4;
	printk(KERN_WARNING "***** IU type = 0x%02x, len = %hd, compat_features = %02x *****\n",
			iu[0], iu_length, iu[1]);
	print_bytes(iu, (int) iu_length, 1, 0);
}

static void __attribute__((unused)) print_unsubmitted_commands(struct pqi_device_queue *q)
{
	u16 pi;
	int i;
	unsigned char *iu;
	unsigned long flags;

	spin_lock_irqsave(&q->index_lock, flags);
	pi = q->local_pi;
	if (pi == q->unposted_index) {
		printk(KERN_WARNING "submit queue is empty.\n");
		spin_unlock_irqrestore(&q->index_lock, flags);
		return;
	}
	if (pi < q->unposted_index) {
		for (i = pi; i < q->unposted_index; i++) {
			iu = (unsigned char *) q->vaddr + (i * IQ_IU_SIZE);
			print_iu(iu);
		}
	} else {
		for (i = pi; i < q->nelements; i++) {
			iu = (unsigned char *) q->vaddr + (i * IQ_IU_SIZE);
			print_iu(iu);
		}
		for (i = 0; i < q->unposted_index; i++) {
			iu = (unsigned char *) q->vaddr + (i * IQ_IU_SIZE);
			print_iu(iu);
		}
	}
	spin_unlock_irqrestore(&q->index_lock, flags);
}

static void pqi_notify_device_queue_written(struct sop_device *h, struct pqi_device_queue *q)
{
	unsigned long flags;
	int curr;
	/*
	 * Notify the device that the host has produced data for the device
	 */

	spin_lock_irqsave(&q->index_lock, flags);
	q->local_pi = q->unposted_index;
	writew(q->unposted_index, q->index.to_dev.pi);
	spin_unlock_irqrestore(&q->index_lock, flags);
	atomic_inc(&h->curr_outstanding_commands);
	spin_lock_irqsave(&h->stat_lock, flags);
	curr = atomic_read(&h->curr_outstanding_commands);
	if (curr > h->max_outstanding_commands)
		h->max_outstanding_commands = curr;
	spin_unlock_irqrestore(&h->stat_lock, flags);
}

static void pqi_notify_device_queue_read(struct pqi_device_queue *q)
{
	/*
	 * Notify the device that the host has consumed data from the device
	 */
	writew(q->unposted_index, q->index.from_dev.ci);
}

static int wait_for_admin_command_ack(struct sop_device *h)
{
	u64 paf;
	u8 function_and_status;
	int count = 0;
	__iomem void *sig = &h->pqireg->signature;

#define ADMIN_SLEEP_INTERVAL_MIN	100 /* microseconds */
#define ADMIN_SLEEP_INTERVAL_MAX	150 /* microseconds */
#define ADMIN_SLEEP_INTERATIONS		1000 /* total of 100 milliseconds */
#define ADMIN_SLEEP_TMO_MS		100 /* total of 100 milliseconds */

	do {
		usleep_range(ADMIN_SLEEP_INTERVAL_MIN,
				ADMIN_SLEEP_INTERVAL_MAX);
		if (safe_readq(sig, &paf, &h->pqireg->process_admin_function)) {
			dev_warn(&h->pdev->dev,
				"%s: Failed to read device memory\n", __func__);
			return -1;
		}
		function_and_status = paf & 0xff;
		if (function_and_status == 0x00)
			return 0;
		count++;
	} while (count < ADMIN_SLEEP_INTERATIONS);
	return -1;
}

static int wait_for_admin_queues_to_become_idle(struct sop_device *h,
						int timeout_ms,
						u8 device_state)
{
	int i;
	u64 paf;
	u32 status;
	u8 pqi_device_state, function_and_status;
	__iomem void *sig = &h->pqireg->signature;
	int tmo_count = timeout_ms * 10;	/* Each loop is 100us */

	for (i = 0; i < tmo_count; i++) {
		usleep_range(ADMIN_SLEEP_INTERVAL_MIN,
				ADMIN_SLEEP_INTERVAL_MAX);
		if (safe_readq(sig, &paf,
				&h->pqireg->process_admin_function)) {
			dev_warn(&h->pdev->dev,
				"Cannot read process admin function register");
			return -1;
		}
		paf &= 0x0ff;
		if (safe_readl(sig, &status, &h->pqireg->pqi_device_status)) {
			dev_warn(&h->pdev->dev,
				"Cannot read device status register");
			return -1;
		}
		function_and_status = paf & 0xff;
		pqi_device_state = status & 0xff;
		if (function_and_status == PQI_IDLE &&
			pqi_device_state == device_state)
			return 0;
		if (i == 0)
			dev_warn(&h->pdev->dev,
				"Waiting for admin queues to become idle (FnSt=0x%x, DevSt=0x%x\n",
				function_and_status, pqi_device_state);
	}
	dev_warn(&h->pdev->dev,
			"Failed waiting for admin queues to become idle and device state %d.",
			device_state);
	return -1;
}

static inline int sop_admin_queue_buflen(struct sop_device *h, int nelements)
{
	return (((h->pqicap.admin_iq_element_length * 16) +
		(h->pqicap.admin_oq_element_length * 16)) *
		nelements + 32);
}

static void sop_free_admin_queues(struct sop_device *h)
{
	struct queue_info *adminq = &h->qinfo[0];
	struct pqi_device_queue *iq;

	free_q_request_buffers(adminq);

	if ((iq = adminq->iq)) {
		/* 
		* For Admin, only a flat bufffer is allocated for PCI 
		* and starts at iq
		*/
		if (iq->vaddr) {
			int total_admin_qsize = sop_admin_queue_buflen(h,
					h->qinfo[0].iq->nelements);

			pci_free_consistent(h->pdev, total_admin_qsize,
					iq->vaddr, iq->dhandle);
		}
		kfree(iq);
	}
	if (adminq->oq)
		kfree(adminq->oq);
}

#define ADMIN_QUEUE_ELEMENT_COUNT 64
static int __devinit sop_alloc_admin_queues(struct sop_device *h)
{
	u64 pqicap;
	u8 admin_iq_elem_count, admin_oq_elem_count;
	__iomem void *sig = &h->pqireg->signature;
	char *msg = "";

	if (safe_readq(sig, &pqicap, &h->pqireg->capability)) {
		dev_warn(&h->pdev->dev,  "Unable to read pqi capability register\n");
		return -1;
	}
	memcpy(&h->pqicap, &pqicap, sizeof(h->pqicap));

	admin_iq_elem_count = admin_oq_elem_count = ADMIN_QUEUE_ELEMENT_COUNT;

	if (h->pqicap.max_admin_iq_elements < admin_iq_elem_count)
		admin_iq_elem_count = h->pqicap.max_admin_iq_elements;
	if (h->pqicap.max_admin_oq_elements < admin_oq_elem_count)
		admin_oq_elem_count = h->pqicap.max_admin_oq_elements;
	if (admin_oq_elem_count == 0 || admin_iq_elem_count == 0) {
		dev_warn(&h->pdev->dev, "Invalid Admin Q elerment count %d in PQI caps\n",
				ADMIN_QUEUE_ELEMENT_COUNT);
		return -1;
	}

	if (pqi_device_queue_alloc(h, &h->qinfo[0].oq, admin_oq_elem_count,
			h->pqicap.admin_iq_element_length,
			PQI_DIR_FROM_DEVICE, 0))
		return -1;

	if (pqi_device_queue_alloc(h, &h->qinfo[0].iq, admin_iq_elem_count,
			h->pqicap.admin_iq_element_length,
			PQI_DIR_TO_DEVICE, 0))
		goto bailout;

#define PQI_REG_ALIGNMENT 16

	if (h->qinfo[0].iq->dhandle % PQI_REG_ALIGNMENT != 0 ||
		h->qinfo[0].oq->dhandle % PQI_REG_ALIGNMENT != 0) {
		dev_warn(&h->pdev->dev, "Admin queues are not properly aligned.\n");
		dev_warn(&h->pdev->dev, "admin_iq_busaddr = %llx\n",
				(unsigned long long)h->qinfo[0].iq->dhandle);
		dev_warn(&h->pdev->dev, "admin_oq_busaddr = %llx\n",
				(unsigned long long)h->qinfo[0].oq->dhandle);
	}
	return 0;

bailout:
	sop_free_admin_queues(h);

	dev_warn(&h->pdev->dev, "%s: %s\n", __func__, msg);
	return -1;
}

static int __devinit sop_create_admin_queues(struct sop_device *h)
{
	u64 paf, admin_iq_pi_offset, admin_oq_ci_offset;
	u32 status, admin_queue_param;
	u8 function_and_status;
	u8 pqi_device_state;
	struct pqi_device_queue *admin_iq, *admin_oq;
	volatile u16 *admin_iq_ci, *admin_oq_pi;
	__iomem u16 *admin_iq_pi, *admin_oq_ci;
	dma_addr_t admin_iq_ci_busaddr, admin_oq_pi_busaddr;
	u16 msix_vector;
	int rc;
	char *msg = "";
	__iomem void *sig = &h->pqireg->signature;
	__iomem void *tmpptr;

	/* Check that device is ready to be set up */
	if (wait_for_admin_queues_to_become_idle(h, ADMIN_SLEEP_TMO_MS,
					PQI_READY_FOR_ADMIN_FUNCTION))
		return -1;

	admin_iq = h->qinfo[0].iq;
	admin_oq = h->qinfo[0].oq;

	admin_iq_ci = admin_iq->index.to_dev.ci;
	admin_oq_pi = admin_oq->index.from_dev.pi;

	admin_iq_ci_busaddr = admin_iq->dhandle +
				(h->pqicap.admin_iq_element_length * 16) *
				admin_iq->nelements;
	admin_oq_pi_busaddr = admin_oq->dhandle +
				(h->pqicap.admin_oq_element_length * 16) *
				admin_oq->nelements;

	msix_vector = 0; /* Admin Queue always uses vector [0] */
	admin_queue_param = ADMIN_QUEUE_ELEMENT_COUNT |
			(ADMIN_QUEUE_ELEMENT_COUNT << 8) |
			(msix_vector << 16);

	/* Tell the hardware about the admin queues */
	writeq(admin_iq->dhandle, &h->pqireg->admin_iq_addr);
	writeq(admin_oq->dhandle, &h->pqireg->admin_oq_addr);
	writeq(admin_iq_ci_busaddr, &h->pqireg->admin_iq_ci_addr);
	writeq(admin_oq_pi_busaddr, &h->pqireg->admin_oq_pi_addr);
	writel(admin_queue_param, &h->pqireg->admin_queue_param);
	writeq(PQI_CREATE_ADMIN_QUEUES, &h->pqireg->process_admin_function);

	rc = wait_for_admin_command_ack(h);
	if (rc) {
		if (safe_readq(sig, &paf, &h->pqireg->process_admin_function)) {
			msg = "Failed reading process admin function register";
			goto bailout;
		}
		function_and_status = paf & 0xff;
		dev_warn(&h->pdev->dev,
			"Failed to create admin queues: function_and_status = 0x%02x\n",
			function_and_status);
		if (function_and_status == 0) {
			msg = "Failed waiting for admin command ack";
			goto bailout;
		}
		if (safe_readl(sig, &status, &h->pqireg->pqi_device_status)) {
			msg = "Failed reading pqi device status register";
			goto bailout;
		}
		dev_warn(&h->pdev->dev, "Device status = 0x%08x\n", status);
	}

	/* Get the offsets of the hardware updated producer/consumer indices */
	if (safe_readq(sig, &admin_iq_pi_offset,
				&h->pqireg->admin_iq_pi_offset)) {
		msg = "Unable to read admin iq pi offset register";
		goto bailout;
	}
	if (safe_readq(sig, &admin_oq_ci_offset,
				&h->pqireg->admin_oq_ci_offset)) {
		msg = "Unable to read admin oq ci offset register";
		goto bailout;
	}

	tmpptr = (__iomem void *) h->pqireg;
	admin_iq_pi = (__iomem u16 *) (tmpptr + admin_iq_pi_offset);
	admin_oq_ci = (__iomem u16 *) (tmpptr + admin_oq_ci_offset);

	/* TODO: why do we read the status register here? block driver doesn't. */
	if (safe_readl(sig, &status, &h->pqireg->pqi_device_status)) {
		msg = "Failed to read device status register";
		goto bailout;
	}
	function_and_status = paf & 0xff;
	pqi_device_state = status & 0xff;

	pqi_device_queue_init(admin_oq, admin_oq_ci, admin_oq_pi,
				PQI_DIR_FROM_DEVICE);
	pqi_device_queue_init(admin_iq, admin_iq_pi, admin_iq_ci,
				PQI_DIR_TO_DEVICE);

	/* Allocate request buffers for admin queues */
	if (allocate_q_request_buffers(&h->qinfo[0],
				ADMIN_QUEUE_ELEMENT_COUNT,
				sizeof(struct sop_request))) {
		msg = "Failed to allocate request queue buffer for queue 0";
		goto bailout;
	}
	return 0;

bailout:
	sop_free_admin_queues(h);

	dev_warn(&h->pdev->dev, "%s: %s\n", __func__, msg);
	return -1;
}

static int sop_delete_admin_queues(struct sop_device *h)
{
	u64 paf;
	u32 status;
	u8 function_and_status;
	__iomem void *sig = &h->pqireg->signature;

	if (wait_for_admin_queues_to_become_idle(h, ADMIN_SLEEP_TMO_MS,
						PQI_READY_FOR_IO))
		return -1;
	writeq(PQI_DELETE_ADMIN_QUEUES, &h->pqireg->process_admin_function);
	if (wait_for_admin_command_ack(h) == 0)
		return 0;

	/* Try to get some clues about why it failed. */
	dev_warn(&h->pdev->dev, "Failed waiting for admin command acknowledgment\n");
	if (safe_readq(sig, &paf,  &h->pqireg->process_admin_function)) {
		dev_warn(&h->pdev->dev,
			"Cannot read process admin function register");
		return -1;
	}
	function_and_status = paf & 0xff;
	dev_warn(&h->pdev->dev,
		"Failed to delete admin queues: function_and_status = 0x%02x\n",
		function_and_status);
	if (function_and_status == 0)
		return -1;
	if (safe_readl(sig, &status, &h->pqireg->pqi_device_status)) {
		dev_warn(&h->pdev->dev,
			"Failed to read device status register");
		return -1;
	}
	dev_warn(&h->pdev->dev, "Device status = 0x%08x\n", status);
	return -1;
}

static int sop_setup_msix(struct sop_device *h)
{
	int i, err;

	struct msix_entry msix_entry[MAX_TOTAL_QUEUE_PAIRS];

	h->nr_queue_pairs = num_online_cpus() + 1;
	if (h->nr_queue_pairs > MAX_TOTAL_QUEUE_PAIRS)
		h->nr_queue_pairs = MAX_TOTAL_QUEUE_PAIRS;

	/*
	 * Set up (h->nr_queue_pairs - 1) msix vectors. -1 because
	 * outbound admin queue shares with io queue 0
	 */
	for (i = 0; i < h->nr_queue_pairs - 1; i++) {
		msix_entry[i].vector = 0;
		msix_entry[i].entry = i;
	}

	err = 0;
	if (!pci_find_capability(h->pdev, PCI_CAP_ID_MSIX))
		goto msix_failed;

	while (1) {
		err = pci_enable_msix(h->pdev, msix_entry,
				h->nr_queue_pairs - 1);
		if (err == 0)
			break;	/* Success */
		if (err < 0)
			goto msix_failed;

		/* Try reduced number of vectors */
		dev_warn(&h->pdev->dev,
			"Requested %d MSI-X vectors, available %d\n",
			h->nr_queue_pairs - 1, err);
		h->nr_queue_pairs = err + 1;
	}
	for (i = 0; i < h->nr_queue_pairs; i++) {
		/* vid makes admin q share with io q 0 */
		int vid = i ? i - 1 : 0;
		h->qinfo[i].msix_entry = msix_entry[vid].entry;
		h->qinfo[i].msix_vector = msix_entry[vid].vector;
		dev_warn(&h->pdev->dev, "q[%d] msix_entry[%d] = %d\n",
			i, vid, msix_entry[vid].vector);
	}
	h->intr_mode = INTR_MODE_MSIX;
	return 0;

msix_failed:
	/* Use regular interrupt */
	h->nr_queue_pairs = 2;

	h->qinfo[0].msix_entry = 0;
	h->qinfo[1].msix_entry = 1;
	h->qinfo[0].msix_vector = h->qinfo[1].msix_vector = h->pdev->irq;
	h->intr_mode = INTR_MODE_INTX;

	dev_warn(&h->pdev->dev, "MSI-X init failed (using legacy intr): %s\n",
		err ?	"failed to enable MSI-X" :
			"device does not support MSI-X");
	return 0;
}

/* function to determine whether a complete response has been accumulated */
static int sop_response_accumulated(struct sop_request *r)
{
	u16 iu_length;

	if (r->response_accumulated == 0)
		return 0;
	iu_length = le16_to_cpu(*(u16 *) &r->response[2]) + 4;
	return (r->response_accumulated >= iu_length);
}

static void free_request(struct sop_device *h, u8 queue_pair_index, u16 request_id);

static void main_io_path_decode_response_data(struct sop_device *h,
					struct sop_cmd_response *scr,
					struct scsi_cmnd *scmd)
{
	char *msg;
	u8 firmware_bug = 0;

	switch (scr->response[3]) {
	case SOP_TMF_COMPLETE:
	case SOP_TMF_REJECTED:
	case SOP_TMF_FAILED:
	case SOP_TMF_SUCCEEDED:
		/*
		 * There should be no way to submit a Task Management Function
		 * IU via the main i/o path, so don't expect TMF response data.
		 */
		msg = "Received TMF response in main i/o path.\n";
		firmware_bug = 1;
		break;
	case SOP_INCORRECT_LUN:
		msg = "Incorrect LUN response.\n";
		break;
	case SOP_OVERLAPPED_REQUEST_ID_ATTEMPTED:
		msg = "Overlapped request ID attempted.\n";
		break;
	case SOP_INVALID_IU_TYPE:
		msg = "Invaid IU type";
		break;
	case SOP_INVALID_IU_LENGTH:
		msg = "Invalid IU length";
		break;
	case SOP_INVALID_LENGTH_IN_IU:
		msg = "Invalid length in IU";
		break;
	case SOP_MISALIGNED_LENGTH_IN_IU:
		msg = "Misaligned length in IU";
		break;
	case SOP_INVALID_FIELD_IN_IU:
		msg = "Invalid field in IU";
		break;
	case SOP_IU_TOO_LONG:
		msg = "IU too long";
		break;
	default:
		msg = "Unknown response type";
	}
	scmd->result |= (DID_ERROR << 16);
	dev_warn(&h->pdev->dev,
		"Unexpected response in main i/o path: %s. Suspect %s bug.\n",
		msg, firmware_bug ? "firmware" : "driver");
	return;
}

static void handle_management_response(struct sop_device *h, 
					struct management_response_iu * mr,
					struct scsi_cmnd *scmd)
{
	switch (mr->result) {
	case MGMT_RSP_RSLT_GOOD:
		scsi_set_resid(scmd, 0); /* right??? */
		dev_warn(&h->pdev->dev,
			"Management IU response: good result\n");
		return;
	case MGMT_RSP_RSLT_UNKNOWN_ERROR:
		dev_warn(&h->pdev->dev,
			"Management IU response: unknown error\n");
		break;
	case MGMT_RSP_RSLT_INVALID_FIELD_IN_REQUEST_IU:
		dev_warn(&h->pdev->dev,
			"Management IU response: Invalid field in request IU\n");
		break;
	case MGMT_RSP_RSLT_INVALID_FIELD_IN_DATA_OUT_BUFFER:
		dev_warn(&h->pdev->dev,
			"Management IU response: Invalid field in data out buffer\n");
		break;
	case MGMT_RSP_RSLT_VENDOR_SPECIFIC_ERROR:
	case MGMT_RSP_RSLT_VENDOR_SPECIFIC_ERROR2:
		dev_warn(&h->pdev->dev,
			"Management IU response: vendor specific error\n");
		break;
	default:
		dev_warn(&h->pdev->dev,
			"Management IU response: unknown response: %02x\n",
				mr->result);
		break;
	}
	scmd->result |= (DID_ERROR << 16);
}

static void complete_scsi_cmd(struct sop_device *h, struct queue_info *qinfo,
				struct sop_request *r)
{
	struct scsi_cmnd *scmd;
	struct sop_cmd_response *scr;
	struct management_response_iu *mr;
	u16 sense_data_len;
	u16 response_data_len;
	u32 data_xferred;

        scmd = r->scmd;
        scsi_dma_unmap(scmd); /* undo the DMA mappings */

        scmd->result = (DID_OK << 16);           /* host byte */
        scmd->result |= (COMMAND_COMPLETE << 8); /* msg byte */
	free_request(h, qinfo_to_qid(qinfo), r->request_id);

	switch (r->response[0]) {
	case SOP_RESPONSE_CMD_SUCCESS_IU_TYPE:
		scsi_set_resid(scmd, 0);
		break;
	case SOP_RESPONSE_CMD_RESPONSE_IU_TYPE:
		scr = (struct sop_cmd_response *) r->response;
		scmd->result |= scr->status;
		sense_data_len = le16_to_cpu(scr->sense_data_len);
		response_data_len = le16_to_cpu(scr->response_data_len);
		if (unlikely(response_data_len && sense_data_len))
			dev_warn(&h->pdev->dev,
				"Both sense and response data not expected.\n");

		/* copy the sense data */
		if (sense_data_len) {
			if (SCSI_SENSE_BUFFERSIZE < sense_data_len)
				sense_data_len = SCSI_SENSE_BUFFERSIZE;
			memset(scmd->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
			memcpy(scmd->sense_buffer, scr->sense, sense_data_len);
		}

		/* paranoia, check for out of spec firmware */
		if (scr->data_in_xfer_result && scr->data_out_xfer_result)
			dev_warn(&h->pdev->dev,
				"Unexpected bidirectional cmd with status in and out\n");

		/* Calculate residual count */
		if (scr->data_in_xfer_result)
			data_xferred = le32_to_cpu(scr->data_in_xferred);
		else
			if (scr->data_out_xfer_result)
				data_xferred = le32_to_cpu(scr->data_out_xferred);
			else
				data_xferred = r->xfer_size;
		scsi_set_resid(scmd, r->xfer_size - data_xferred);

		if (response_data_len)
			main_io_path_decode_response_data(h, scr, scmd);
		break;
	case SOP_RESPONSE_TASK_MGMT_RESPONSE_IU_TYPE:
		scmd->result |= (DID_ERROR << 16);
		dev_warn(&h->pdev->dev, "got unhandled response type...\n");
		break;
	case SOP_RESPONSE_MANAGEMENT_RESPONSE_IU_TYPE:
		/* FIXME: how the heck are we even getting in here? */
		mr = (struct management_response_iu *) r->response;
		handle_management_response(h, mr, scmd);
		break;
	default:
		scmd->result |= (DID_ERROR << 16);
		dev_warn(&h->pdev->dev, "got UNKNOWN response type...\n");
		break;
	}
	scmd->scsi_done(scmd);
}

irqreturn_t sop_ioq_msix_handler(int irq, void *devid)
{
	u16 request_id;
	u8 iu_type;
	int rc;
	struct queue_info *q = devid;
	struct sop_device *h = q->h;
#if 0
	printk(KERN_WARNING "=========> Got ioq interrupt, q = %p (%d) vector = %d\n",
			q, q->pqiq->queue_id, q->msix_vector);
#endif
	do {
		struct sop_request *r = q->oq->cur_req;

		if (pqi_from_device_queue_is_empty(q->oq)) {
			/* dev_warn(&h->pdev->dev, "==== interrupt, ioq %d is empty ====\n",
					q->pqiq->queue_id); */
			break;
		}

		if (r == NULL) {
			/* Receiving completion of a new request */ 
			iu_type = pqi_peek_ui_type_from_device(q->oq);
			request_id = pqi_peek_request_id_from_device(q->oq);
			r = q->oq->cur_req = &q->request[request_id];
			r->request_id = request_id;
			r->response_accumulated = 0;
		}
		rc = pqi_dequeue_from_device(q->oq,
				&r->response[r->response_accumulated]); 
		if (rc) { /* queue is empty */
			dev_warn(&h->pdev->dev, "=-=-=- io OQ[%hhu] PI %d CI %d is empty(rc = %d)\n",
				q->oq->queue_id, *(q->oq->index.from_dev.pi), q->oq->unposted_index, rc);
			return IRQ_HANDLED;
		}
		r->response_accumulated += q->oq->element_size;
		/* dev_warn(&h->pdev->dev, "accumulated %d bytes\n", r->response_accumulated); */
		if (sop_response_accumulated(r)) {
			/* dev_warn(&h->pdev->dev, "accumlated response\n"); */
			q->oq->cur_req = NULL;
			wmb();
			WARN_ON((!r->waiting && !r->scmd));
			if (likely(r->scmd)) {
				complete_scsi_cmd(h, q, r);
				r = NULL;
			} else {
				if (likely(r->waiting)) {
					dev_warn(&h->pdev->dev, "Unexpected, waiting != NULL\n");
					complete(r->waiting);
					r = NULL;
				} else {
					dev_warn(&h->pdev->dev, "r->scmd and r->waiting both null\n");
				}
			}
			pqi_notify_device_queue_read(q->oq);
			atomic_dec(&h->curr_outstanding_commands);
		}
	} while (!pqi_from_device_queue_is_empty(q->oq));

	return IRQ_HANDLED;
}

irqreturn_t sop_adminq_msix_handler(int irq, void *devid)
{
	struct queue_info *q = devid;
	u8 iu_type;
	u16 request_id;
	int rc;
	struct sop_device *h = q->h;

	do {
		struct sop_request *r = q->oq->cur_req;

		if (pqi_from_device_queue_is_empty(q->oq))
			return IRQ_NONE;

		if (r == NULL) {
			/* Receiving completion of a new request */ 
			iu_type = pqi_peek_ui_type_from_device(q->oq);
			request_id = pqi_peek_request_id_from_device(q->oq);
			r = q->oq->cur_req = &q->request[request_id];
			r->response_accumulated = 0;
		}
		rc = pqi_dequeue_from_device(q->oq, &r->response[r->response_accumulated]); 
		if (rc) /* queue is empty */
			return IRQ_HANDLED;
		r->response_accumulated += q->oq->element_size;
		if (sop_response_accumulated(r)) {
			q->oq->cur_req = NULL;
			wmb();
			complete(r->waiting);
			pqi_notify_device_queue_read(q->oq);
			atomic_dec(&h->curr_outstanding_commands);
		}
	} while (!pqi_from_device_queue_is_empty(q->oq));

	return IRQ_HANDLED;
}

static void sop_irq_affinity_hints(struct sop_device *h)
{
	int i, cpu;

	cpu = cpumask_first(cpu_online_mask);
	for (i = 1; i < h->nr_queue_pairs; i++) {
		int rc;
		rc = irq_set_affinity_hint(h->qinfo[i].msix_vector,
					get_cpu_mask(cpu));

		if (rc)
			dev_warn(&h->pdev->dev, "Failed to hint affinity of vector %d to cpu %d\n",
					h->qinfo[i].msix_vector, cpu);
		cpu = cpumask_next(cpu, cpu_online_mask);
	}
}

static int sop_request_irq(struct sop_device *h, int queue_index,
				irq_handler_t msix_handler)
{
	int rc;

	rc = request_irq(h->qinfo[queue_index].msix_vector, msix_handler,
				IRQF_SHARED, h->devname, &h->qinfo[queue_index]);
	if (rc != 0)
		dev_warn(&h->pdev->dev, "Request_irq failed, queue_index = %d\n",
				queue_index);
	return rc;
}

static int sop_request_io_irqs(struct sop_device *h,
				irq_handler_t msix_handler)
{
	int i;

	for (i = 1; i < h->nr_queue_pairs; i++) {
		if (sop_request_irq(h, i, msix_handler))
			goto irq_fail;
	}
	sop_irq_affinity_hints(h);
	return 0;
irq_fail:
	/* Free all the irqs already allocated */
	while (--i >= 0)
		free_irq(h->qinfo[i].msix_vector, &h->qinfo[i]);
	return -1;
}

static void sop_free_irq(struct sop_device *h,  int qinfo_ind)
{
	int vector;

	vector = h->qinfo[qinfo_ind].msix_vector;
	irq_set_affinity_hint(vector, NULL);
	free_irq(vector, &h->qinfo[qinfo_ind]);
}

static void sop_free_io_irqs(struct sop_device *h)
{
	int i;

	for (i = 1; i < h->nr_queue_pairs; i++)
		sop_free_irq(h, i);
}

static void sop_free_admin_irq_and_disable_msix(struct sop_device *h)
{
	sop_free_irq(h, 0);
#ifdef CONFIG_PCI_MSI
	if (h->intr_mode == INTR_MODE_MSIX && h->pdev->msix_enabled)
		pci_disable_msix(h->pdev);
#endif /* CONFIG_PCI_MSI */
}

/* FIXME: maybe there's a better way to do this */
static int alloc_request(struct sop_device *h, u8 queue_pair_index)
{
	int rc;
	struct queue_info *qinfo = &h->qinfo[queue_pair_index];

	BUG_ON(qinfo->qdepth > h->elements_per_io_queue);

        do {
                rc = (u16) find_first_zero_bit(qinfo->request_bits, qinfo->qdepth);
                if (rc >= qinfo->qdepth - 1) {
			dev_warn(&h->pdev->dev, "alloc_request failed.\n");
			return -EBUSY;
		}
        } while (test_and_set_bit((int) rc, qinfo->request_bits));
	return rc;
}

static void free_request(struct sop_device *h, u8 queue_pair_index, u16 request_id)
{
	BUG_ON(request_id >= h->qinfo[queue_pair_index].qdepth);
	clear_bit(request_id, h->qinfo[queue_pair_index].request_bits);
}

static void fill_create_io_queue_request(struct sop_device *h,
	struct pqi_create_operational_queue_request *r,
	struct pqi_device_queue *q, int to_device, u16 request_id,
	u16 msix_vector)
{
	u8 function_code;

	if (to_device)
		function_code = CREATE_QUEUE_TO_DEVICE;
	else 
		function_code = CREATE_QUEUE_FROM_DEVICE;

	memset(r, 0, sizeof(*r));
	r->iu_type = OPERATIONAL_QUEUE_IU_TYPE;
	r->iu_length = cpu_to_le16(0x003c);
	r->response_oq = 0;
	r->request_id = request_id;
	r->function_code = function_code;
	r->queue_id = cpu_to_le16(q->queue_id);
	r->element_array_addr = cpu_to_le64(q->dhandle);
	r->index_addr = cpu_to_le64(q->dhandle +
			q->nelements * q->element_size);
	r->nelements = cpu_to_le16((u16) q->nelements);
	r->element_length = cpu_to_le16((u16) (q->element_size/16));
	if (to_device) {
		r->iqp.operational_queue_protocol = 0;
	} else {
		r->oqp.interrupt_message_number = cpu_to_le16(msix_vector);
		/* Coalascing is not supported yet */
		r->oqp.operational_queue_protocol = 0;
	}
}

static void fill_delete_io_queue_request(struct sop_device *h,
	struct pqi_delete_operational_queue_request *r, u16 queue_id,
	int to_device, u16 request_id)
{
	u8 function_code;

	if (to_device)
		function_code = DELETE_QUEUE_TO_DEVICE;
	else 
		function_code = DELETE_QUEUE_FROM_DEVICE;

	memset(r, 0, sizeof(*r));
	r->iu_type = OPERATIONAL_QUEUE_IU_TYPE;
	r->iu_length = cpu_to_le16(0x003c);
	r->request_id = request_id;
	r->function_code = function_code;
	r->queue_id = cpu_to_le16(queue_id);
}

static void send_admin_command(struct sop_device *h, u16 request_id)
{
	struct sop_request *request;
	struct queue_info *qinfo = &h->qinfo[0];
	DECLARE_COMPLETION_ONSTACK(wait);

	request = &qinfo->request[request_id];
	request->waiting = &wait;
	request->response_accumulated = 0;
	pqi_notify_device_queue_written(h, qinfo->iq);
	wait_for_completion(&wait);
}

static void send_sop_command(struct sop_device *h, struct queue_info *qinfo,
				u16 request_id)
{
	struct sop_request *sopr;
	DECLARE_COMPLETION_ONSTACK(wait);

	sopr = &qinfo->request[request_id];
	memset(sopr, 0, sizeof(*sopr));
	sopr->request_id = request_id;
	sopr->waiting = &wait;
	sopr->response_accumulated = 0;
	pqi_notify_device_queue_written(h, qinfo->iq);
	put_cpu();
	wait_for_completion(&wait);
}

static int sop_create_io_queue(struct sop_device *h, struct queue_info *q,
				int queue_pair_index, int direction)
{
	struct pqi_device_queue *aq = h->qinfo[0].iq;
	struct pqi_device_queue *ioq;
	struct pqi_create_operational_queue_request *r;
	int request_id;
	volatile struct pqi_create_operational_queue_response *resp;
	__iomem u16 *pi_or_ci;

	if (direction == PQI_DIR_FROM_DEVICE) {
		ioq = q->oq;
	} else {
		ioq = q->iq;
	}
	spin_lock_init(&ioq->index_lock);
	spin_lock_init(&ioq->qlock);
	r = pqi_alloc_elements(aq, 1);
	request_id = alloc_request(h, 0);
	dev_warn(&h->pdev->dev, "Allocated request %hu, %p\n", request_id,
			&h->qinfo[aq->queue_id].request[request_id]);
	if (request_id < 0) {
		dev_warn(&h->pdev->dev, "Requests exhausted for create Q #%d\n",
			queue_pair_index);
		goto bail_out;
	}
	fill_create_io_queue_request(h, r, ioq,
					direction == PQI_DIR_TO_DEVICE,
					request_id, q->msix_entry);
	send_admin_command(h, request_id);
	resp = (volatile struct pqi_create_operational_queue_response *)
		h->qinfo[0].request[request_id].response;	
	if (resp->status != 0) {
		dev_warn(&h->pdev->dev, "Failed to create OQ #%d\n",
			queue_pair_index);
		free_request(h, 0, request_id);
		goto bail_out;
	}

	pi_or_ci = ((__iomem void *) h->pqireg) +
					le64_to_cpu(resp->index_offset);
	if (direction == PQI_DIR_TO_DEVICE)
		pqi_device_queue_init(ioq, pi_or_ci, ioq->index.to_dev.ci,
			direction);
	else
		pqi_device_queue_init(ioq, pi_or_ci, ioq->index.from_dev.pi,
			direction);
	free_request(h, 0, request_id);
	return 0;
bail_out:
	return -1;
}

static void sop_free_io_queues(struct sop_device *h)
{
	int i;

	for (i = 1; i < h->nr_queue_pairs; i++) {
		struct queue_info *qinfo = &h->qinfo[i];

		pqi_device_queue_free(h, qinfo->iq);
		pqi_device_queue_free(h, qinfo->oq);
		pqi_iq_buffer_free(h, qinfo);
	}
}

static int sop_setup_io_queue_pairs(struct sop_device *h)
{
	int i;

	/* From 1, not 0, to skip admin oq, which was already set up */
	for (i = 1; i < h->nr_queue_pairs; i++) {
		if (pqi_device_queue_alloc(h, &h->qinfo[i].oq,
				h->elements_per_io_queue, IQ_IU_SIZE / 16, PQI_DIR_FROM_DEVICE, i))
			goto bail_out;
		if (pqi_device_queue_alloc(h, &h->qinfo[i].iq,
				h->elements_per_io_queue, OQ_IU_SIZE / 16, PQI_DIR_TO_DEVICE, i))
			goto bail_out;
		if (pqi_iq_data_alloc(h, &h->qinfo[i]))
			goto bail_out;
		if (sop_create_io_queue(h, &h->qinfo[i], i, PQI_DIR_FROM_DEVICE))
			goto bail_out;
		if (sop_create_io_queue(h, &h->qinfo[i], i, PQI_DIR_TO_DEVICE))
			goto bail_out;
	}
	return 0;

bail_out:
	sop_free_io_queues(h);
	return -1;
}

static int fill_get_pqi_device_capabilities(struct sop_device *h,
			struct report_pqi_device_capability_iu *r,
			u16 request_id, void *buffer, u32 buffersize)
{
	u64 busaddr;

	memset(r, 0, sizeof(*r));
	r->iu_type = REPORT_PQI_DEVICE_CAPABILITY;
	r->compatible_features = 0;
	r->iu_length = cpu_to_le16(sizeof(*r) - PQI_IU_HEADER_SIZE);;
	r->response_oq = 0;
	r->work_area = 0;
	r->request_id = request_id;
	r->function_code = 0;
	r->buffer_size = cpu_to_le32(buffersize);

        busaddr = pci_map_single(h->pdev, buffer, buffersize,
                                PCI_DMA_FROMDEVICE);
	if (dma_mapping_error(&h->pdev->dev, busaddr))
		return -ENOMEM;
	r->sg.address = cpu_to_le64(busaddr);
	r->sg.length = cpu_to_le32(buffersize);
	r->sg.descriptor_type = PQI_SGL_DATA_BLOCK;
	return 0;
}

static int sop_get_pqi_device_capabilities(struct sop_device *h)
{
	struct report_pqi_device_capability_iu *r;
	volatile struct report_pqi_device_capability_response *resp;
	struct pqi_device_queue *aq = h->qinfo[0].iq;
	struct pqi_device_capabilities *buffer;
	int request_id = (u16) -EBUSY, rc = 0;
	u64 busaddr;

	h->elements_per_io_queue = DRIVER_MAX_IQ_NELEMENTS;
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities\n");
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 2\n");
	r = pqi_alloc_elements(aq, 1);
	if (IS_ERR(r)) {
		dev_warn(&h->pdev->dev, "pqi_alloc_elements failed\n");
		rc = PTR_ERR(r);
		goto error;
	}
	request_id = alloc_request(h, 0);
	if (request_id == (u16) -EBUSY) {
		dev_warn(&h->pdev->dev, "alloc_request failed\n");
		rc = -ENOMEM;
		goto error;
	}
	if (fill_get_pqi_device_capabilities(h, r, request_id, buffer,
						(u32) sizeof(*buffer))) {
		/* we have to submit request (already in queue) but it
		 * is now a NULL IU, and will be ignored by hardware.
		 */
		dev_warn(&h->pdev->dev,
			"pci_map_single failed in fill_get_pqi_device_capabilities\n");
		dev_warn(&h->pdev->dev,
			"Sending NULL IU, this code is untested.\n");
		free_request(h, aq->queue_id, request_id);
		pqi_notify_device_queue_written(h, aq);
		goto error;
	}
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 3\n");
	send_admin_command(h, request_id);
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 4\n");
	busaddr = le64_to_cpu(r->sg.address);
	pci_unmap_single(h->pdev, busaddr, sizeof(*buffer),
						PCI_DMA_FROMDEVICE);
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 5\n");
	resp = (volatile struct report_pqi_device_capability_response *)
			h->qinfo[0].request[request_id].response;
	if (resp->status != 0) {
		dev_warn(&h->pdev->dev, "resp->status = %d\n", resp->status);	
		free_request(h, 0, request_id);
		goto error;
	}
	free_request(h, 0, request_id);
	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 6\n");
	h->max_iqs = le16_to_cpu(buffer->max_iqs);
	h->max_iq_elements = le16_to_cpu(buffer->max_iq_elements);
	h->max_iq_element_length = le16_to_cpu(buffer->max_iq_element_length);
	h->min_iq_element_length = le16_to_cpu(buffer->min_iq_element_length);
	h->max_oqs = le16_to_cpu(buffer->max_oqs);
	h->max_oq_elements = le16_to_cpu(buffer->max_oq_elements);
	h->max_oq_element_length = le16_to_cpu(buffer->max_oq_element_length);
	h->min_oq_element_length = le16_to_cpu(buffer->min_oq_element_length);
	h->intr_coalescing_time_granularity =
		le16_to_cpu( buffer->intr_coalescing_time_granularity);
	h->iq_alignment_exponent = buffer->iq_alignment_exponent;
	h->oq_alignment_exponent = buffer->oq_alignment_exponent;
	h->iq_ci_alignment_exponent = buffer->iq_ci_alignment_exponent;
	h->oq_pi_alignment_exponent = buffer->oq_pi_alignment_exponent;
	h->protocol_support_bitmask =
		le32_to_cpu(buffer->protocol_support_bitmask);
	h->admin_sgl_support_bitmask =
		le16_to_cpu(buffer->admin_sgl_support_bitmask);

	dev_warn(&h->pdev->dev, "Getting pqi device capabilities 7:\n");

	dev_warn(&h->pdev->dev, "max iqs = %hu\n", h->max_iqs);
	dev_warn(&h->pdev->dev, "max iq_elements = %hu\n", h->max_iq_elements);
	dev_warn(&h->pdev->dev, "max iq_element_length = %hu\n", h->max_iq_element_length);
	dev_warn(&h->pdev->dev, "min iq_element_length = %hu\n", h->min_iq_element_length);
	dev_warn(&h->pdev->dev, "max oqs = %hu\n", h->max_oqs);
	dev_warn(&h->pdev->dev, "max oq_elements = %hu\n", h->max_oq_elements);
	dev_warn(&h->pdev->dev, "max oq_element_length = %hu\n", h->max_oq_element_length);
	dev_warn(&h->pdev->dev, "min oq_element_length = %hu\n", h->min_oq_element_length);
	dev_warn(&h->pdev->dev, "intr_coalescing_time_granularity = %hu\n", h->intr_coalescing_time_granularity);
	dev_warn(&h->pdev->dev, "iq_alignment_exponent = %hhu\n", h->iq_alignment_exponent);
	dev_warn(&h->pdev->dev, "oq_alignment_exponent = %hhu\n", h->oq_alignment_exponent);
	dev_warn(&h->pdev->dev, "iq_ci_alignment_exponent = %hhu\n", h->iq_ci_alignment_exponent);
	dev_warn(&h->pdev->dev, "oq_pi_alignment_exponent = %hhu\n", h->oq_pi_alignment_exponent);
	dev_warn(&h->pdev->dev, "protocol support bitmask = 0x%08x\n", h->protocol_support_bitmask);
	dev_warn(&h->pdev->dev, "admin_sgl_support_bitmask = 0x%04x\n", h->admin_sgl_support_bitmask);

	h->elements_per_io_queue = DRIVER_MAX_IQ_NELEMENTS;
	if (h->elements_per_io_queue > DRIVER_MAX_OQ_NELEMENTS)
		h->elements_per_io_queue = DRIVER_MAX_OQ_NELEMENTS;
	if (h->elements_per_io_queue > h->max_oq_elements)
		h->elements_per_io_queue = h->max_oq_elements;
	if (h->elements_per_io_queue > h->max_iq_elements)
		h->elements_per_io_queue = h->max_iq_elements;

	dev_warn(&h->pdev->dev, "elements per i/o queue: %d\n",
			h->elements_per_io_queue);

	kfree(buffer);
	return 0;

error:
	if (request_id != (u16) -EBUSY)
		free_request(h, 0, request_id);
	kfree(buffer);
	return rc;
}

static int sop_delete_io_queue(struct sop_device *h, int qpindex, int to_device)
{
	struct pqi_delete_operational_queue_request *r;
	struct pqi_device_queue *aq = h->qinfo[0].iq;
	int request_id;
	volatile struct pqi_delete_operational_queue_response *resp;
	u16 qid;
	int err = 0;

	/* Check to see if the Admin queue is ready for taking commands */
	if (wait_for_admin_queues_to_become_idle(h, ADMIN_SLEEP_TMO_MS,
							PQI_READY_FOR_IO))
		return -ENODEV;

	r = pqi_alloc_elements(aq, 1);
	request_id = alloc_request(h, 0);
	if (request_id < 0) {
		dev_warn(&h->pdev->dev, "Requests unexpectedly exhausted\n");
		err = -ENOMEM;
		goto bail_out;
	}
	qid = qpindex_to_qid(qpindex, to_device);
	fill_delete_io_queue_request(h, r, qid, to_device, request_id);
	send_admin_command(h, request_id);
	resp = (volatile struct pqi_delete_operational_queue_response *)
		h->qinfo[0].request[request_id].response;
	if (resp->status != 0) {
		dev_warn(&h->pdev->dev, "Failed to tear down OQ... now what?\n");
		err = -EIO;
	}

	free_request(h, 0, request_id);
bail_out:
	return err;
}

static int sop_delete_io_queues(struct sop_device *h)
{
	int i;

	for (i = 1; i < h->nr_queue_pairs; i++) {
		if (sop_delete_io_queue(h, i, 1))
			break;
		if (sop_delete_io_queue(h, i, 0))
			break;
	}
	sop_free_io_queues(h);
	return 0;
}

static int sop_set_dma_mask(struct pci_dev * pdev)
{
	int rc;

	if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(64)) &&
		!pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)))
		return 0;
	rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (!rc)
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	return rc;
}

static int sop_register_host(struct sop_device *h)
{
	struct Scsi_Host *sh;
	int rc;

	sh = scsi_host_alloc(&sop_template, sizeof(h));
	if (!sh)
		goto bail;
	sh->io_port = 0;
	sh->n_io_port = 0;
	sh->this_id = -1;
	sh->max_channel = 1;
	sh->max_cmd_len = MAX_COMMAND_SIZE;
	sh->max_lun = 1; /* FIXME are these correct? */
	sh->max_id = 1;
	sh->can_queue = h->elements_per_io_queue;
	sh->cmd_per_lun = h->elements_per_io_queue;
	sh->sg_tablesize = MAX_SGLS; /* FIXME make this bigger */
	sh->hostdata[0] = (unsigned long) h;
	sh->irq = h->qinfo[0].msix_vector;
	sh->unique_id = sh->irq; /* really? */
	h->scsi_host = sh;
	rc = scsi_add_host(sh, &h->pdev->dev);
	if (rc)
		goto add_host_failed;
	scsi_scan_host(sh);
	return 0;

add_host_failed:
	dev_err(&h->pdev->dev, "scsi_add_host failed.\n");
	scsi_host_put(sh);
	return rc;
bail:
	dev_err(&h->pdev->dev, "scsi_host_alloc failed.\n");
	return -ENOMEM;
}

#define PQI_RESET_ACTION_SHIFT 5
#define PQI_RESET_ACTION_MASK (0x07 << PQI_RESET_ACTION_SHIFT)
#define PQI_START_RESET (1 << PQI_RESET_ACTION_SHIFT)
#define PQI_SOFT_RESET (1)
#define PQI_START_RESET_COMPLETED (2 << PQI_RESET_ACTION_SHIFT)

static int sop_wait_for_host_reset_ack(struct sop_device *h, uint tmo_ms)
{
	u32 reset_register, prev;
	int count = 0;
	int tmo_iter = tmo_ms * 10;	/* Each iteration is 100 us */

	prev = -1;
	do {
		usleep_range(ADMIN_SLEEP_INTERVAL_MIN,
				ADMIN_SLEEP_INTERVAL_MAX);
		/* 
		 * Not using safe_readl here because while in reset we can
		 * get -1 and be unable to read the signature, and this
		 * is normal (I think).
		 */
		reset_register = readl(&h->pqireg->reset);
		if (reset_register != prev)
			dev_warn(&h->pdev->dev, "Reset register is: 0x%08x\n",
				reset_register);
		prev = reset_register;
		if ((reset_register & PQI_RESET_ACTION_MASK) ==
					PQI_START_RESET_COMPLETED)
			return 0;
		count++;
	} while (count < tmo_iter);

	return -1;
}

#define ADMIN_RESET_TMO_MS		3000
static int sop_init_time_host_reset(struct sop_device *h)
{
	u64 paf;
	u32 status;
	u8 pqi_device_state, function_and_status;
	__iomem void *sig = &h->pqireg->signature;

	dev_warn(&h->pdev->dev, "Resetting host\n");
	writel(PQI_START_RESET | PQI_SOFT_RESET, &h->pqireg->reset);

	if (sop_wait_for_host_reset_ack(h, ADMIN_RESET_TMO_MS))
		return -1;

	dev_warn(&h->pdev->dev, "Host reset initiated.\n");
	do {
		if (safe_readq(sig, &paf, &h->pqireg->process_admin_function)) {
			dev_warn(&h->pdev->dev,
				"Unable to read process admin function register");
			return -1;
		}
		if (safe_readl(sig, &status, &h->pqireg->pqi_device_status)) {
			dev_warn(&h->pdev->dev, "Unable to read from device memory");
			return -1;
		}
		function_and_status = paf & 0xff;
		pqi_device_state = status & 0xff;
		usleep_range(ADMIN_SLEEP_INTERVAL_MIN,
				ADMIN_SLEEP_INTERVAL_MAX);
	} while (pqi_device_state != PQI_READY_FOR_ADMIN_FUNCTION ||
			function_and_status != PQI_IDLE);
	dev_warn(&h->pdev->dev, "Host reset completed.\n");
	return 0;
}

static int __devinit sop_probe(struct pci_dev *pdev,
			const struct pci_device_id *pci_id)
{
	struct sop_device *h;
	u64 signature;
	int i, rc;
	__iomem void *sig;

	dev_warn(&pdev->dev, SOP "found device: %04x:%04x/%04x:%04x\n",
			pdev->vendor, pdev->device,
			pdev->subsystem_vendor, pdev->subsystem_device);

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->max_outstanding_commands = 0;
	atomic_set(&h->curr_outstanding_commands, 0);
	spin_lock_init(&h->stat_lock);
	h->ctlr = controller_num;
	for (i = 0; i < MAX_TOTAL_QUEUE_PAIRS; i++)
		h->qinfo[i].h = h;
	controller_num++;
	sprintf(h->devname, "sop-%d\n", h->ctlr);

	h->pdev = pdev;
	pci_set_drvdata(pdev, h);

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_warn(&h->pdev->dev, "Unable to enable PCI device\n");
		goto bail_set_drvdata;
	}

	/* Enable bus mastering (pci_disable_device may disable this) */
	pci_set_master(h->pdev);

	rc = pci_request_regions(h->pdev, SOP);
	if (rc) {
		dev_err(&h->pdev->dev,
			"Cannot obtain PCI resources, aborting\n");
		goto bail_pci_enable;
	}

	h->pqireg = pci_ioremap_bar(pdev, 0);
	if (!h->pqireg) {
		rc = -ENOMEM;
		goto bail_request_regions;
	}
	rc = sop_init_time_host_reset(h);
	if (rc) {
		dev_warn(&h->pdev->dev, "Failed to Reset Device\n");
		goto bail_remap_bar;
	}

	sig = &h->pqireg->signature;

	if (sop_set_dma_mask(pdev)) {
		dev_err(&pdev->dev, "Failed to set DMA mask\n");
		goto bail_remap_bar;
	}

	if (safe_readq(sig, &signature, &h->pqireg->signature)) {
		dev_warn(&pdev->dev, "Unable to read PQI signature\n");
		goto bail_remap_bar;
	}
	if (memcmp("PQI DREG", &signature, sizeof(signature)) != 0) {
		dev_warn(&pdev->dev, "Device does not appear to be a PQI device\n");
		goto bail_remap_bar;
	}

	rc = sop_setup_msix(h);
	if (rc != 0)
		goto bail_remap_bar;

	rc = sop_alloc_admin_queues(h);
	if (rc)
		goto bail_enable_msix;

	rc = sop_create_admin_queues(h);
	if (rc)
		goto bail_enable_msix;

	rc = sop_request_irq(h, 0, sop_adminq_msix_handler);
	if (rc != 0) {
		dev_warn(&h->pdev->dev, "Bailing out in probe - requesting IRQ[0]\n");
		goto bail_admin_created;
	}

	rc = sop_get_pqi_device_capabilities(h);
	if (rc) {
		dev_warn(&h->pdev->dev, "Bailing out in probe - getting device capabilities\n");
		goto bail_admin_irq;
	}

	rc = sop_setup_io_queue_pairs(h);
	if (rc) {
		dev_warn(&h->pdev->dev, "Bailing out in probe - Creating i/o queues\n");
		goto bail_admin_irq;
	}

	rc = sop_request_io_irqs(h, sop_ioq_msix_handler);
	if (rc)
		goto bail_io_q_created;

	rc = sop_register_host(h);
	if (rc)
		goto bail_io_irq;

	return 0;

bail_io_irq:
	for (i=1; i< h->nr_queue_pairs; i++)
		sop_free_irq(h, 0);
bail_io_q_created:
	sop_delete_io_queues(h);
bail_admin_irq:
	/* Free admin irq */
	sop_free_irq(h, 0);
bail_admin_created:
	sop_delete_admin_queues(h);
bail_enable_msix:
	pci_disable_msix(pdev);
bail_remap_bar:
	if (h && h->pqireg)
		iounmap(h->pqireg);
bail_request_regions:
	pci_release_regions(pdev);
bail_pci_enable:
	pci_disable_device(pdev);
bail_set_drvdata:
	pci_set_drvdata(pdev, NULL);
	kfree(h);
	return -1;
}

static int sop_suspend(__attribute__((unused)) struct pci_dev *pdev,
				__attribute__((unused)) pm_message_t state)
{
	return -ENOSYS;
}

static int sop_resume(__attribute__((unused)) struct pci_dev *pdev)
{
	return -ENOSYS;
}

static void __devexit sop_remove(struct pci_dev *pdev)
{
	struct sop_device *h;

	h = pci_get_drvdata(pdev);
	dev_warn(&h->pdev->dev, "remove called.\n");
        scsi_remove_host(h->scsi_host);
        scsi_host_put(h->scsi_host);
        h->scsi_host = NULL;
	sop_free_io_irqs(h);
	sop_delete_io_queues(h);
	sop_delete_admin_queues(h);
	sop_free_admin_irq_and_disable_msix(h);
	if (h && h->pqireg)
		iounmap(h->pqireg);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(h);
}

static void sop_shutdown(struct pci_dev *pdev)
{
	dev_warn(&pdev->dev, "shutdown called.\n");
}

static struct pci_driver sop_pci_driver = {
	.name = SOP,
	.probe = sop_probe,
	.remove = __devexit_p(sop_remove),
	.id_table = sop_id_table,
	.shutdown = sop_shutdown,
	.suspend = sop_suspend,
	.resume = sop_resume,
	.err_handler = &sop_pci_error_handlers,
};

static int __init sop_init(void)
{
	return pci_register_driver(&sop_pci_driver);
}

static void __exit sop_exit(void)
{
	pci_unregister_driver(&sop_pci_driver);
}

static inline struct sop_device *sdev_to_hba(struct scsi_device *sdev)
{
	unsigned long *priv = shost_priv(sdev->host);
	return (struct sop_device *) *priv;
}

static inline int find_sop_queue(struct sop_device *h, int cpu)
{
	return 1 + (cpu % (h->nr_queue_pairs - 1));
}

static void fill_sg_data_element(struct pqi_sgl_descriptor *sgld,
				struct scatterlist *sg, u32 *xfer_size)
{
	sgld->address = cpu_to_le64(sg_dma_address(sg));
	sgld->length = cpu_to_le32(sg_dma_len(sg));
	*xfer_size += sg_dma_len(sg);
	sgld->descriptor_type = PQI_SGL_DATA_BLOCK;
}

static void fill_sg_chain_element(struct pqi_sgl_descriptor *sgld,
				struct queue_info *q,
				int sg_block_number, int sg_count)
{
	sgld->address = cpu_to_le64(q->sg_bus_addr + sg_block_number *
				sizeof(*sgld));
	sgld->length = cpu_to_le32(sg_count * sizeof(*sgld));
	sgld->descriptor_type = PQI_SGL_STANDARD_LAST_SEG;
}

static void fill_inline_sg_list(struct sop_limited_cmd_iu *r,
				struct scsi_cmnd *sc, int use_sg,
				u32 *xfer_size)
{
	struct pqi_sgl_descriptor *datasg;
	struct scatterlist *sg;
	int i;
	static const u16 no_sgl_size =
			(u16) (sizeof(*r) - sizeof(r->sg[0]) * 2) - 4;
	BUILD_BUG_ON(sizeof(*r) != 64);
	BUILD_BUG_ON((sizeof(r->sg[0]) != (16)));
	BUILD_BUG_ON((sizeof(*r) - sizeof(r->sg[0]) * 2) != 32);

	BUG_ON(use_sg > 2);
	if (!use_sg) {
		r->iu_length = cpu_to_le16(no_sgl_size);
		return;
	}
	r->iu_length = cpu_to_le16(no_sgl_size + sizeof(*datasg) * use_sg);
	*xfer_size = 0;
	datasg = &r->sg[0];
	scsi_for_each_sg(sc, sg, use_sg, i) {
		fill_sg_data_element(datasg, sg, xfer_size);
		datasg++;
	}
}

static int sop_scatter_gather(struct sop_device *h,
			struct queue_info *q, 
			struct sop_limited_cmd_iu *r,
			struct scsi_cmnd *sc, u32 *xfer_size)
{
	struct scatterlist *sg;
	int sg_block_number;
	int i, j, use_sg;
	struct pqi_sgl_descriptor *datasg;
	static const u16 no_sgl_size =
			(u16) (sizeof(*r) - sizeof(r->sg[0]) * 2) - 4;

	BUG_ON(scsi_sg_count(sc) > MAX_SGLS);

	use_sg = scsi_dma_map(sc);
	if (use_sg < 0)
		return use_sg;

	if (use_sg < 3) {
		fill_inline_sg_list(r, sc, use_sg, xfer_size);
		return 0;
	}

	sg_block_number = r->request_id * MAX_SGLS;
	*xfer_size = 0;
	r->iu_length = cpu_to_le16(no_sgl_size + sizeof(*datasg) * 2);
	datasg = &r->sg[0];
	j = 0;
	scsi_for_each_sg(sc, sg, use_sg, i) {
		if (j == 1) {
			fill_sg_chain_element(datasg, q,
					sg_block_number, scsi_sg_count(sc) - 1);
			datasg = &q->sg[sg_block_number];
			j++;
		}
		fill_sg_data_element(datasg, sg, xfer_size);
		datasg++;
		j++;
	}
	return 0;
}

static int sop_queuecommand(struct Scsi_Host *shost, struct scsi_cmnd *sc)
{
	struct sop_device *h;
	/* struct scsi_device *sdev = sc->device; */
	int queue_pair_index;
	struct queue_info *qinfo;
	struct sop_limited_cmd_iu *r;
	struct sop_request *sopr;
	int request_id;
	int cpu;

	h = sdev_to_hba(sc->device);

	/* reject io to devices other than b0t0l0 */
	if (sc->device->channel != 0 || sc->device->id != 0 || 
		sc->device->lun != 0) {
                sc->result = DID_NO_CONNECT << 16;
                sc->scsi_done(sc);
                return 0;
	}

	cpu = get_cpu();
	queue_pair_index = find_sop_queue(h, cpu);
	qinfo = &h->qinfo[queue_pair_index];
	spin_lock_irq(&qinfo->iq->qlock);
	if (!qinfo)
		dev_warn(&h->pdev->dev, "queuecommand: q is null!\n");
	if (!qinfo->iq)
		dev_warn(&h->pdev->dev, "queuecommand: q->iq is null!\n");
	r = pqi_alloc_elements(qinfo->iq, 1);
	if (IS_ERR(r)) {
		dev_warn(&h->pdev->dev, "pqi_alloc_elements returned %ld\n", PTR_ERR(r));
	}
	request_id = alloc_request(h, queue_pair_index);
	if (request_id < 0)
		dev_warn(&h->pdev->dev, "Failed to allocate request! Trouble ahead.\n");

	r->iu_type = SOP_LIMITED_CMD_IU;
	r->compatible_features = 0;
	r->queue_id = cpu_to_le16(queue_pair_index);
	r->work_area = 0;
	r->request_id = request_id;
	sopr = &qinfo->request[request_id];
	sopr->xfer_size = 0;
	sopr->scmd = sc;
	sc->host_scribble = (unsigned char *) sopr;
	sopr->waiting = NULL;

	switch (sc->sc_data_direction) {
	case DMA_TO_DEVICE:
		r->flags = SOP_DATA_DIR_TO_DEVICE;
		break;
	case DMA_FROM_DEVICE:
		r->flags = SOP_DATA_DIR_FROM_DEVICE;
		break;
	case DMA_NONE:
		r->flags = SOP_DATA_DIR_NONE;
		break;
	case DMA_BIDIRECTIONAL:
		r->flags = SOP_DATA_DIR_RESERVED;
		break;
	}
	memset(r->cdb, 0, 16);
	memcpy(r->cdb, sc->cmnd, sc->cmd_len);
	if (sop_scatter_gather(h, qinfo, r, sc, &sopr->xfer_size)) {
		/*
		 * Scatter gather mapping failed.  Tell midlayer to back off.
		 * There's no "unallocating" from the submit ring buffer,
		 * so just make it a null IU and deallocate the corresponding
		 * request.
		 */
		memset(r, 0, 4); /* NULL IU */
		free_request(h, queue_pair_index, request_id);
		pqi_notify_device_queue_written(h, qinfo->iq);
		spin_unlock_irq(&qinfo->iq->qlock);
		put_cpu();
		return SCSI_MLQUEUE_HOST_BUSY;
	}
	r->xfer_size = cpu_to_le32(sopr->xfer_size);
	pqi_notify_device_queue_written(h, qinfo->iq);
	spin_unlock_irq(&qinfo->iq->qlock);
	put_cpu();
	return 0;
}

static int sop_change_queue_depth(struct scsi_device *sdev,
        int qdepth, int reason)
{
	struct sop_device *h = sdev_to_hba(sdev);

	dev_warn(&h->pdev->dev, "sop_change_queue_depth called but not implemented\n");
	return 0;
}

static void fill_task_mgmt_request(struct sop_task_mgmt_iu *tm,
		struct queue_info *q, u16 request_id,
		u16 request_id_to_manage, u8 task_mgmt_function)
{
	memset(tm, 0, sizeof(*tm));
	tm->iu_type = SOP_TASK_MGMT_IU;
	tm->iu_length = cpu_to_le16(0x001C);
	tm->queue_id = cpu_to_le16(q->iq->queue_id);
	tm->request_id = request_id;
	tm->nexus_id = 0;
	tm->lun = 0;
	tm->request_id_to_manage = request_id_to_manage;
	tm->task_mgmt_function = task_mgmt_function;
}

static int process_task_mgmt_response(struct sop_device *h,
			struct queue_info *qinfo, u16 request_id)
{
	struct sop_request *sopr = &qinfo->request[request_id];
	struct sop_task_mgmt_response *tmr =
		(struct sop_task_mgmt_response *) sopr->response;
	u8 response_code;

	if (tmr->iu_type != SOP_RESPONSE_TASK_MGMT_RESPONSE_IU_TYPE)
		dev_warn(&h->pdev->dev, "Unexpected IU type %hhu in %s\n",
				tmr->iu_type, __func__);
	response_code = tmr->response_code;
	free_request(h, qinfo->oq->queue_id, request_id);
	switch (response_code) {
	case SOP_TMF_COMPLETE:
	case SOP_TMF_SUCCEEDED:
	case SOP_TMF_REJECTED:
		return SUCCESS;
	}
	return FAILED;
}

static int sop_abort_handler(struct scsi_cmnd *sc)
{
	struct sop_device *h;
	struct sop_request *sopr_to_abort =
			(struct sop_request *) sc->host_scribble;
	struct queue_info *q;
	struct sop_task_mgmt_iu *abort_cmd;
	int queue_pair_index, request_id, cpu;

	h = sdev_to_hba(sc->device);

	dev_warn(&h->pdev->dev, "sop_abort_handler: this code is UNTESTED.\n");
	cpu = get_cpu();
	queue_pair_index = find_sop_queue(h, cpu);
	q = &h->qinfo[queue_pair_index];
	spin_lock_irq(&q->iq->qlock);
	abort_cmd = pqi_alloc_elements(q->iq, 1);
	if (IS_ERR(abort_cmd)) {
		dev_warn(&h->pdev->dev, "%s: pqi_alloc_elements returned %ld\n",
				__func__, PTR_ERR(abort_cmd));
		spin_unlock_irq(&q->iq->qlock);
		put_cpu();
		return FAILED;
	}
	request_id = alloc_request(h, queue_pair_index);
	if (request_id < 0) {
		dev_warn(&h->pdev->dev, "%s: Failed to allocate request\n",
					__func__);
		/* don't free it, just let it be a NULL IU */
		spin_unlock_irq(&q->iq->qlock);
		put_cpu();
		return FAILED;
	}
	fill_task_mgmt_request(abort_cmd, q, request_id,
				sopr_to_abort->request_id, SOP_ABORT_TASK);
	send_sop_command(h, q, request_id);
	spin_unlock_irq(&q->iq->qlock);
	return process_task_mgmt_response(h, q, request_id);
}

static int sop_device_reset_handler(struct scsi_cmnd *sc)
{
	struct sop_device *h;
	struct sop_request *sopr_to_reset =
			(struct sop_request *) sc->host_scribble;
	struct queue_info *q;
	struct sop_task_mgmt_iu *reset_cmd;
	int queue_pair_index, request_id, cpu;

	h = sdev_to_hba(sc->device);

	dev_warn(&h->pdev->dev, "sop_device_reset_handler: this code is UNTESTED.\n");
	cpu = get_cpu();
	queue_pair_index = find_sop_queue(h, cpu);
	q = &h->qinfo[queue_pair_index];
	reset_cmd = pqi_alloc_elements(q->iq, 1);
	if (IS_ERR(reset_cmd)) {
		dev_warn(&h->pdev->dev, "%s: pqi_alloc_elements returned %ld\n",
				__func__, PTR_ERR(reset_cmd));
		return FAILED;
	}
	request_id = alloc_request(h, queue_pair_index);
	if (request_id < 0) {
		dev_warn(&h->pdev->dev, "%s: Failed to allocate request\n",
					__func__);
		/* don't free it, just let it be a NULL IU */
		return FAILED;
	}
	fill_task_mgmt_request(reset_cmd, q, request_id,
				sopr_to_reset->request_id, SOP_LUN_RESET);
	send_sop_command(h, q, request_id);
	return process_task_mgmt_response(h, q, request_id);
}

static int sop_slave_alloc(struct scsi_device *sdev)
{
	/* struct sop_device *h = sdev_to_hba(sdev); */

	/* dev_warn(&h->pdev->dev, "sop_slave_alloc called but not implemented\n"); */
	return 0;
}

static void sop_slave_destroy(struct scsi_device *sdev)
{
	/* struct sop_device *h = sdev_to_hba(sdev); */

	/* dev_warn(&h->pdev->dev, "sop_slave_destroy called but not implemented\n"); */
	return;
}

static int sop_compat_ioctl(struct scsi_device *sdev,
						int cmd, void *arg)
{
	struct sop_device *h = sdev_to_hba(sdev);

	dev_warn(&h->pdev->dev, "sop_compat_ioctl called but not implemented\n");
	return 0;
}

static int sop_ioctl(struct scsi_device *sdev, int cmd, void *arg)
{
	struct sop_device *h = sdev_to_hba(sdev);

	dev_warn(&h->pdev->dev, "sop_ioctl called but not implemented, cmd = 0x%08x\n", cmd);
	return -ENOTTY;
}

static pci_ers_result_t sop_pci_error_detected(struct pci_dev *dev,
				enum pci_channel_state error)
{
	dev_warn(&dev->dev,
		"sop_pci_error_detected called but not implemented\n");
	/* FIXME: implement this. */
	return PCI_ERS_RESULT_NONE;
}

static pci_ers_result_t sop_pci_mmio_enabled(struct pci_dev *dev)
{
	dev_warn(&dev->dev,
		"sop_pci_error_mmio_enabled called but not implemented\n");
	/* FIXME: implement this. */
	return PCI_ERS_RESULT_NONE;
}

static pci_ers_result_t sop_pci_link_reset(struct pci_dev *dev)
{
	dev_warn(&dev->dev,
		"sop_pci_error_link_reset called but not implemented\n");
	/* FIXME: implement this. */
	return PCI_ERS_RESULT_NONE;
}

static pci_ers_result_t sop_pci_slot_reset(struct pci_dev *dev)
{
	dev_warn(&dev->dev,
		"sop_pci_error_slot_reset called but not implemented\n");
	/* FIXME: implement this. */
	return PCI_ERS_RESULT_NONE;
}

static void sop_pci_resume(struct pci_dev *dev)
{
	dev_warn(&dev->dev, "sop_pci_resume called but not implemented\n");
	/* FIXME: implement this. */
	return;
}

/* This gets optimized away, but will fail to compile if we mess up
 * the structure definitions.
 */
static void __attribute__((unused)) verify_structure_defs(void)
{
#define VERIFY_OFFSET(member, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_create_operational_queue_request, \
				member) != offset)

	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2); 
	VERIFY_OFFSET(response_oq, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(function_code, 10);
	VERIFY_OFFSET(reserved2, 11);
	VERIFY_OFFSET(queue_id, 12);
	VERIFY_OFFSET(reserved3, 14);
	VERIFY_OFFSET(element_array_addr, 16);
	VERIFY_OFFSET(index_addr, 24);
	VERIFY_OFFSET(nelements, 32);
	VERIFY_OFFSET(element_length, 34);
	VERIFY_OFFSET(iqp, 36); 
	VERIFY_OFFSET(oqp, 36); 
	VERIFY_OFFSET(reserved4, 47);
#undef VERIFY_OFFSET
	BUILD_BUG_ON(sizeof(struct pqi_create_operational_queue_request) !=
				64);

#define VERIFY_OFFSET(member, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_create_operational_queue_response, \
		member) != offset)

	VERIFY_OFFSET(ui_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(ui_length, 2);
	VERIFY_OFFSET(response_oq, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(function_code, 10);
	VERIFY_OFFSET(status, 11);
	VERIFY_OFFSET(reserved2, 12);
	VERIFY_OFFSET(index_offset, 16);
	VERIFY_OFFSET(reserved3, 24);
	BUILD_BUG_ON(sizeof(struct pqi_create_operational_queue_response) !=
				64);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_device_register_set, \
			field) != offset)

	VERIFY_OFFSET(signature, 0);
	VERIFY_OFFSET(process_admin_function, 0x08);
	VERIFY_OFFSET(capability, 0x10);
	VERIFY_OFFSET(legacy_intx_status, 0x18);
	VERIFY_OFFSET(legacy_intx_mask_set, 0x1c);
	VERIFY_OFFSET(legacy_intx_mask_clear, 0x20);
	VERIFY_OFFSET(pqi_device_status, 0x40);
	VERIFY_OFFSET(admin_iq_pi_offset, 0x48);
	VERIFY_OFFSET(admin_oq_ci_offset, 0x50);
	VERIFY_OFFSET(admin_iq_addr, 0x58);
	VERIFY_OFFSET(admin_oq_addr, 0x60);
	VERIFY_OFFSET(admin_iq_ci_addr, 0x68);
	VERIFY_OFFSET(admin_oq_pi_addr, 0x70);
	VERIFY_OFFSET(admin_queue_param, 0x78);
	VERIFY_OFFSET(device_error, 0x80);
	VERIFY_OFFSET(error_data, 0x88);
	VERIFY_OFFSET(reset, 0x90);
	VERIFY_OFFSET(power_action, 0x94);

#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_delete_operational_queue_request, \
			field) != offset)
        VERIFY_OFFSET(iu_type, 0);
        VERIFY_OFFSET(compatible_features, 1);
        VERIFY_OFFSET(iu_length, 2);
        VERIFY_OFFSET(response_oq, 4);
        VERIFY_OFFSET(work_area, 6);
        VERIFY_OFFSET(request_id, 8);
        VERIFY_OFFSET(function_code, 10);
        VERIFY_OFFSET(reserved2, 11);
        VERIFY_OFFSET(queue_id, 12);
        VERIFY_OFFSET(reserved3, 14);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_delete_operational_queue_response, \
			field) != offset)
 	VERIFY_OFFSET(ui_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(ui_length, 2);
	VERIFY_OFFSET(response_oq, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(function_code, 10);
	VERIFY_OFFSET(status, 11);
	VERIFY_OFFSET(reserved2, 12);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_sgl_descriptor, field) != offset)
	VERIFY_OFFSET(address, 0);
	VERIFY_OFFSET(length, 8);
	VERIFY_OFFSET(reserved, 12);
	VERIFY_OFFSET(descriptor_type, 15);
#undef VERIFY_OFFSET
	BUILD_BUG_ON(sizeof(struct pqi_sgl_descriptor) != 16);

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct sop_limited_cmd_iu, field) != offset)
	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(flags, 10);
	VERIFY_OFFSET(reserved, 11);
	VERIFY_OFFSET(xfer_size, 12);
	VERIFY_OFFSET(cdb, 16);
	VERIFY_OFFSET(sg, 32);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct sop_cmd_response, field) != offset)
	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(nexus_id, 10);
	VERIFY_OFFSET(data_in_xfer_result, 12);
	VERIFY_OFFSET(data_out_xfer_result, 13);
	VERIFY_OFFSET(reserved, 14);
	VERIFY_OFFSET(status, 17);
	VERIFY_OFFSET(status_qualifier, 18);
	VERIFY_OFFSET(sense_data_len, 20);
	VERIFY_OFFSET(response_data_len, 22);
	VERIFY_OFFSET(data_in_xferred, 24);
	VERIFY_OFFSET(data_out_xferred, 28);
	VERIFY_OFFSET(response, 32);
	VERIFY_OFFSET(sense, 32);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct report_pqi_device_capability_iu, field) != offset)

	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(response_oq, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(function_code, 10);
	VERIFY_OFFSET(reserved, 11);
	VERIFY_OFFSET(buffer_size, 44);
	VERIFY_OFFSET(sg, 48);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct report_pqi_device_capability_response, field) != offset)
	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(function_code, 10);
	VERIFY_OFFSET(status, 11);
	VERIFY_OFFSET(additional_status, 12);
	VERIFY_OFFSET(reserved, 16);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct pqi_device_capabilities, field) != offset)
	VERIFY_OFFSET(length, 0);
	VERIFY_OFFSET(reserved, 2);
	VERIFY_OFFSET(max_iqs, 16);
	VERIFY_OFFSET(max_iq_elements, 18);
	VERIFY_OFFSET(reserved2, 20);
	VERIFY_OFFSET(max_iq_element_length, 24);
	VERIFY_OFFSET(min_iq_element_length, 26);
	VERIFY_OFFSET(max_oqs, 28);
	VERIFY_OFFSET(max_oq_elements, 30);
	VERIFY_OFFSET(reserved3, 32);
	VERIFY_OFFSET(intr_coalescing_time_granularity, 34);
	VERIFY_OFFSET(max_oq_element_length, 36);
	VERIFY_OFFSET(min_oq_element_length, 38);
	VERIFY_OFFSET(iq_alignment_exponent, 40);
	VERIFY_OFFSET(oq_alignment_exponent, 41);
	VERIFY_OFFSET(iq_ci_alignment_exponent, 42);
	VERIFY_OFFSET(oq_pi_alignment_exponent, 43);
	VERIFY_OFFSET(protocol_support_bitmask, 44);
	VERIFY_OFFSET(admin_sgl_support_bitmask, 48);
	VERIFY_OFFSET(reserved4, 50);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct sop_task_mgmt_iu, field) != offset)

	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(nexus_id, 10);
	VERIFY_OFFSET(reserved, 12);
	VERIFY_OFFSET(lun, 16);
	VERIFY_OFFSET(protocol_specific, 24);
	VERIFY_OFFSET(reserved2, 26);
	VERIFY_OFFSET(request_id_to_manage, 28);
	VERIFY_OFFSET(task_mgmt_function, 30);
	VERIFY_OFFSET(reserved3, 31);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct sop_task_mgmt_response, field) != offset)
	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(nexus_id, 10);
	VERIFY_OFFSET(additional_response_info, 12);
	VERIFY_OFFSET(response_code, 15);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct report_general_iu, field) != offset)
	VERIFY_OFFSET(iu_type, 0);
	VERIFY_OFFSET(compatible_features, 1);
	VERIFY_OFFSET(iu_length, 2);
	VERIFY_OFFSET(queue_id, 4);
	VERIFY_OFFSET(work_area, 6);
	VERIFY_OFFSET(request_id, 8);
	VERIFY_OFFSET(reserved, 10);
	VERIFY_OFFSET(allocation_length, 12);
	VERIFY_OFFSET(reserved2, 16);
	VERIFY_OFFSET(data_in, 32);
#undef VERIFY_OFFSET

#define VERIFY_OFFSET(field, offset) \
	BUILD_BUG_ON(offsetof(struct report_general_response_iu, field) != offset)
		VERIFY_OFFSET(reserved, 0);
		VERIFY_OFFSET(lun_bridge_present_flags, 4);
		VERIFY_OFFSET(reserved2, 5);
		VERIFY_OFFSET(app_clients_present_flags, 8);
		VERIFY_OFFSET(reserved3, 9);
		VERIFY_OFFSET(max_incoming_iu_size, 18);
		VERIFY_OFFSET(max_incoming_embedded_data_buffers, 20);
		VERIFY_OFFSET(max_data_buffers, 22);
		VERIFY_OFFSET(reserved4, 24);
		VERIFY_OFFSET(incoming_iu_type_support_bitmask, 32);
		VERIFY_OFFSET(vendor_specific, 64);
		VERIFY_OFFSET(reserved5, 72);
		VERIFY_OFFSET(queuing_layer_specific_data_len, 74);
		VERIFY_OFFSET(incoming_sgl_support_bitmask, 76);
		VERIFY_OFFSET(reserved6, 78);
#undef VERIFY_OFFSET

}

module_init(sop_init);
module_exit(sop_exit);

