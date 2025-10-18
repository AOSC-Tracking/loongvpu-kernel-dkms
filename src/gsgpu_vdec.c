// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include "gsgpu_vdec.h"
#include "kcl_helper.h"

#ifdef VDEC_TEST_TIME
static struct timeval end_time;
#endif
static gsgpu_vdec_t gsgpu_vdec_data;
static u32 gsgpu_op_instance;
static u32 gsgpu_vdec_instance;
static int gsgpu_vdec_major;
static struct class *gsgpu_vdec_class;
static struct device *gsgpu_vdec_device;
static size_t blocks;
DEFINE_SPINLOCK(mem_lock);
static mem_block_t mem_blocks[256];
#define BLOCK_MAX_PAGES	1999
#define BLOCK_MAX_SIZE	BLOCK_MAX_PAGES * PAGE_SIZE
#define VDEC_DRIVER_DATE "20250312"
unsigned int mem_table[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1,
	4, 4, 4, 4, 4, 4, 4, 4,
	10, 10, 10, 10,
	22, 22, 22, 22,
	38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
	50, 50, 50, 50, 50, 50, 50,
	75, 75, 75, 75, 75,
	86, 86, 86, 86, 86,
	113, 113,
	152, 152,
	162, 162, 162,
	270, 270, 270,
	403, 403, 403, 403,
	403, 403,
	450, 450,
	893, 893,
	893, 893,
	BLOCK_MAX_PAGES, BLOCK_MAX_PAGES
};

static int alloc_memory(unsigned int *busaddr, unsigned int size, struct file *filp)
{
	int i;

	*busaddr = 0;
	for (i = 0; i < blocks; i++) {
		if (!mem_blocks[i].used && (mem_blocks[i].size >= size)) {
			*busaddr = mem_blocks[i].addr;
			mem_blocks[i].used = 1;
			mem_blocks[i].id = *((int *) (filp->private_data));
			break;
		}
	}

	if (*busaddr == 0) {
		printk("alloc failed.\n");
		return -1;
	}

	return 0;
}

static int free_memory(unsigned int busaddr)
{
	int i;

	for (i = 0; i < blocks; i++) {
		if (mem_blocks[i].addr == busaddr) {
			mem_blocks[i].used = 0;
			mem_blocks[i].id = ID_INVALID;
		}
	}

	return 0;
}

static int memalloc_release(struct inode *inode, struct file *filp)
{
	int i;

	for (i = 0; i < blocks; i++) {
			mem_blocks[i].used = 0;
			mem_blocks[i].id = ID_INVALID;
	}
	*((int *)filp->private_data) = ID_INVALID;

	return 0;
}

static int alloc_pages_poll(void)
{
	int i;

	for (i = 0; i < blocks; i++) {
		mem_blocks[i].size = PAGE_SIZE * mem_table[i];
		mem_blocks[i].addr = __pa(alloc_pages_exact(mem_blocks[i].size, GFP_DMA32 | __GFP_ZERO));
		if (!mem_blocks[i].addr) {
			return -ENOMEM;
		}
		mem_blocks[i].used = 0;
		mem_blocks[i].id = ID_INVALID;
	}
	return 0;
}

static int memory_poll_alloc(void)
{
	int result;

	blocks = (sizeof(mem_table) / sizeof(*mem_table));
	result = alloc_pages_poll();
	if (result) {
		return -ENOMEM;
	}

	return 0;
}

static void free_pages_poll(void)
{
	int i;

	for (i = 0; i < blocks; i++) {
		mem_blocks[i].used = 0;
		mem_blocks[i].id = ID_INVALID;
		free_pages_exact(__va((unsigned long )mem_blocks[i].addr), mem_blocks[i].size);
	}
	return;
}

static void memalloc_remove(void)
{
	free_pages_poll();
}

static long gsgpu_vdec_unlock_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
#ifdef VDEC_TEST_TIME
	struct timeval *ts;
#endif
	unsigned long addr;
	int err = 0;

	if (_IOC_NR(cmd) > GSGPU_VDEC_IOC_MAXNR || _IOC_TYPE(cmd) != GSGPU_VDEC_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case GSGPU_VDEC_INIT:
		filp->private_data = &gsgpu_op_instance;
		break;
	case GSGPU_VDEC_IO_BASE:
		__put_user(gsgpu_vdec_data.addr, (unsigned long *) arg);
		break;
	case GSGPU_VDEC_IO_SIZE:
		__put_user(gsgpu_vdec_data.size, (unsigned int *) arg);
		break;
	case GSGPU_VDEC_CLI:
		disable_irq(gsgpu_vdec_data.irq);
		break;
	case GSGPU_VDEC_STI:
		enable_irq(gsgpu_vdec_data.irq);
		break;
#ifdef VDEC_TEST_TIME
	case GSGPU_VDEC_GET_TIME:
		ts = (struct timeval *) arg;
		ts->tv_sec = end_time.tv_sec;
		ts->tv_usec = end_time.tv_usec;
		break;
#endif
	case GSGPU_VDEC_ALLOC:
		{
		int result;
		mem_param_t memparams;

		spin_lock(&mem_lock);

		result = __copy_from_user(&memparams, (const void *) arg, sizeof(memparams));
		if (result) {
			spin_unlock(&mem_lock);
			return -EFAULT;
		}

		if (memparams.size > BLOCK_MAX_SIZE) {
			spin_unlock(&mem_lock);
			return -ENOMEM;
		}
		result = alloc_memory(&memparams.addr, memparams.size, filp);
		if (result) {
			spin_unlock(&mem_lock);
			return result;
		}
		result = __copy_to_user((void *) arg, &memparams, sizeof(memparams));
		if (result) {
			spin_unlock(&mem_lock);
			return -EFAULT;
		}
		spin_unlock(&mem_lock);
		return result;
		}
	case GSGPU_VDEC_FREE:
		spin_lock(&mem_lock);

		__get_user(addr, (unsigned int *) arg);
		err = free_memory(addr);

		spin_unlock(&mem_lock);
		return err;
	}

	return 0;
}

static int gsgpu_vdec_open(struct inode *inode, struct file *filp)
{
	filp->private_data = &gsgpu_vdec_instance;

	return 0;
}

static int gsgpu_vdec_mmap(struct file *f, struct vm_area_struct *vma)
{
	size_t len = vma->vm_end - vma->vm_start;
	unsigned long pfn = vma->vm_pgoff;

	if (!pfn) {
		if (len > gsgpu_vdec_data.size) {
			len = gsgpu_vdec_data.size;
		}
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		pfn = gsgpu_vdec_data.addr >> PAGE_SHIFT;
	}
	return remap_pfn_range(vma, vma->vm_start, pfn,
				len, vma->vm_page_prot);
}

static int gsgpu_vdec_fasync(int fd, struct file *filp, int mode)
{

	gsgpu_vdec_t *dev = &gsgpu_vdec_data;
	struct fasync_struct **async_queue;

	if (((u32 *) filp->private_data) == &gsgpu_vdec_instance) {
		async_queue = &dev->aqv;
	} else {
		async_queue = &dev->aqh;
	}

	return fasync_helper(fd, filp, mode, async_queue);
}

static int gsgpu_vdec_release(struct inode *inode, struct file *filp)
{
	if (filp->f_flags & FASYNC) {
		gsgpu_vdec_fasync(-1, filp, 0);
	}

	memalloc_release(inode, filp);

	return 0;
}

static irqreturn_t gsgpu_vdec_isr(int irq, void *dev_id)
{
	gsgpu_vdec_t *dev = (gsgpu_vdec_t *) dev_id;
	unsigned int handled = 0;

	u32 irq_regv, irq_regh;

	irq_regv = readl(dev->regs + GSGPU_VDEC_V);
	irq_regh = readl(dev->regs + GSGPU_VDEC_H);

	if (irq_regv & GSGPU_VINT) {

#ifdef VDEC_TEST_TIME
		do_gettimeofday(&end_time);
#endif
		writel(irq_regv & (~GSGPU_VINT),
				dev->regs + GSGPU_VDEC_V);
		if (dev->aqv != NULL)
			kill_fasync(&dev->aqv, SIGIO, POLL_IN);

		handled = 1;
	}

	if (irq_regh & GSGPU_HINT) {

#ifdef VDEC_TEST_TIME
		do_gettimeofday(&end_time);
#endif
		writel(irq_regh & (~GSGPU_HINT),
				dev->regs + GSGPU_VDEC_H);
		if (dev->aqh != NULL)
			kill_fasync(&dev->aqh, SIGIO, POLL_IN);

		handled = 1;
	}

	return IRQ_RETVAL(handled);
}

static void HwInit(gsgpu_vdec_t *dev)
{
	int i;

	writel(0, dev->regs + 0x04);
	for (i = 4; i < dev->size; i += 4)
		writel(0, dev->regs + i);
}

static void show_regs(unsigned long data)
{
	gsgpu_vdec_t *dev = (gsgpu_vdec_t *) data;
	int i;

	for (i = 0; i < dev->size; i += 4)
		pr_debug("\tReg Off %02X = %08X\n", i, readl(dev->regs + i));
}

static const struct file_operations gsgpu_vdec_fops = {
	.open = gsgpu_vdec_open,
	.release = gsgpu_vdec_release,
	.unlocked_ioctl = gsgpu_vdec_unlock_ioctl,
	.mmap = gsgpu_vdec_mmap,
	.fasync = gsgpu_vdec_fasync,
};

static const struct of_device_id gsgpu_vdec_dt_match[] = {
	{ .compatible = "loongson,ls-vpu", },
	{},
};
MODULE_DEVICE_TABLE(of, gsgpu_vdec_dt_match);

#if defined(LG_CLASS_DEVNODE_HAS_CONST)
static char *gsgpu_vdec_devnode(const struct device *dev, umode_t *mode)
#else
static char *gsgpu_vdec_devnode(struct device *dev, umode_t *mode)
#endif
{
	if (mode)
		*mode = 0666;
	return kasprintf(GFP_KERNEL, "%s", dev_name(dev));
}

static int device_init(void *dev, bool pci_device)
{
	dev_t devt;
	int ret, irq;
	struct pci_dev *pcidev;
	struct device *device;
	struct resource res, *resp;
	struct platform_device *pdev;

	gsgpu_vdec_data.aqv = NULL;
	gsgpu_vdec_data.aqh = NULL;

	if (pci_device) {
		pcidev = (struct pci_dev *)dev;
		device = &pcidev->dev;

		ret = pci_enable_device(pcidev);
		if (ret < 0) {
			dev_err(&pcidev->dev, "Cannot enable PCI device\n");
		}

		irq = pcidev->irq;
		if (!irq)
			return -ENODEV;

		ret = pci_request_region(pcidev, 0, "ls-vpu io");
		if (ret < 0) {
			dev_err(&pcidev->dev, "Cannot request region 0.\n");
		}

		resp = &res;
		res.start = pci_resource_start(pcidev, 0);
		res.end = pci_resource_end(pcidev, 0);

		gsgpu_vdec_data.regs = ioremap(res.start, resource_size(&res));
		if (IS_ERR(gsgpu_vdec_data.regs))
			return PTR_ERR(gsgpu_vdec_data.regs);

	} else {
		pdev = (struct platform_device *)dev;
		device = &pdev->dev;
		resp = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		gsgpu_vdec_data.regs = devm_ioremap_resource(&pdev->dev, resp);
		if (IS_ERR(gsgpu_vdec_data.regs))
			return PTR_ERR(gsgpu_vdec_data.regs);

		irq = of_irq_get(pdev->dev.of_node, 0);
		if (irq <= 0) {
			return irq ?: -ENXIO;
		}
	}

	gsgpu_vdec_data.addr = resp->start;
	gsgpu_vdec_data.size = resource_size(resp);

	HwInit(&gsgpu_vdec_data);

	ret = devm_request_irq(device, irq, gsgpu_vdec_isr,
			IRQF_SHARED, "gsgpu_vdec",
			(void *) &gsgpu_vdec_data);
	if (ret) {
		return ret;
	}
	gsgpu_vdec_data.irq = irq;

	ret = register_chrdev(gsgpu_vdec_major, "gsgpu_vdec", &gsgpu_vdec_fops);
	if (ret < 0) {
		return ret;
	}
	gsgpu_vdec_major = ret;

	gsgpu_vdec_class = lg_class_create(THIS_MODULE, "gsgpu_vdec");
	if (IS_ERR(gsgpu_vdec_class)) {
		goto err;
	}

	gsgpu_vdec_class->devnode = gsgpu_vdec_devnode;
	devt = MKDEV(gsgpu_vdec_major, 0);
	gsgpu_vdec_device = device_create(gsgpu_vdec_class, NULL, devt, NULL, "%s", "gsgpu_vdec");
	if (IS_ERR(gsgpu_vdec_device)) {
		class_destroy(gsgpu_vdec_class);
		goto err;
	}

	printk("vdec driver date: %s\n", VDEC_DRIVER_DATE);
	return 0;
err:
	unregister_chrdev(gsgpu_vdec_major, "gsgpu_vdec");
	return ret;
}

static int loongvpu_platform_probe(struct platform_device *pdev)
{
	int ret;

	ret = device_init(pdev, 0);
	if (ret) {
		pr_info("gsgpu_vdec: init device error!%d\n", ret);
		return -1;
	}

	ret = memory_poll_alloc();
	if (ret)
		pr_info("gsgpu_vdec: init memory error!%d\n", ret);

	return ret;
}

static int loongvpu_platform_remove(struct platform_device *pdev)
{
	gsgpu_vdec_t *dev = (gsgpu_vdec_t *) &gsgpu_vdec_data;

	writel(0, dev->regs + GSGPU_VDEC_V);
	writel(0, dev->regs + GSGPU_VDEC_H);

	show_regs((unsigned long) dev);
	device_destroy(gsgpu_vdec_class, MKDEV(gsgpu_vdec_major, 0));
	class_destroy(gsgpu_vdec_class);
	unregister_chrdev(gsgpu_vdec_major, "gsgpu_vdec");

	return 0;
}

static struct platform_driver loongvpu_platform_driver = {
	.probe      = loongvpu_platform_probe,
	.remove     = loongvpu_platform_remove,
	.driver     = {
		.name   = "ls-vpu",
		.of_match_table = of_match_ptr(gsgpu_vdec_dt_match),
	},
};

static int loongson_vpu_pci_probe(struct pci_dev *pdev,
		const struct pci_device_id *ent)
{
	int ret;

	ret = device_init(pdev, 1);
	if (ret) {
		pr_info("gsgpu_vdec: init device error!%d\n", ret);
		return -1;
	}

	ret = memory_poll_alloc();
	if (ret)
		pr_info("gsgpu_vdec: init memory error!%d\n", ret);

	return ret;
}
static struct pci_device_id loongson_vpu_devices[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_LOONGSON, 0x7a16) },
	{ }
};

MODULE_DEVICE_TABLE(pci, loongson_vpu_devices);

static struct pci_driver loongson_vpu_pci_driver = {
	.name       = "ls-vpu-pci",
	.id_table   = loongson_vpu_devices,
	.probe      = loongson_vpu_pci_probe,
	.driver = {
		.bus = &pci_bus_type,
	},
};

static int __init loongvpu_driver_init(void)
{
	int err;

	err = pci_register_driver(&loongson_vpu_pci_driver);
	if (err)
		pr_err("Loongvpu pci driver register err!\n");

	return platform_driver_register(&loongvpu_platform_driver);
}

static void __exit loongvpu_driver_exit(void)
{
	pci_unregister_driver(&loongson_vpu_pci_driver);
	platform_driver_unregister(&loongvpu_platform_driver);
	memalloc_remove();
}

module_init(loongvpu_driver_init)
module_exit(loongvpu_driver_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Loongson");
MODULE_DESCRIPTION("driver module for gsgpu vdec driver");
