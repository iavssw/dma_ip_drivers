#define pr_fmt(fmt)	KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/idr.h>
#include <linux/errno.h>
#include "qdma_p2pdma.h"
#include "qdma_ioctl.h"

#define QDMA_P2PDMA_DRV_BASENAME "qdmap2p"
#define QDMA_P2PDMA_MINOR_START 0
#define QDMA_P2PDMA_NUM_MINORS 32
#define QDMA_P2PDMA_DEFAULT_BAR 2
static DEFINE_IDA(minors_ida);

/**
 * struct p2pdma_device - Per-device context for P2P DMA
 * @pdev: Associated PCI device
 * @minor: Minor device number
 * @devt: Device number (major + minor)
 * @cdev: Character device
 * @chardev: Device node in sysfs
 * @bar: BAR number used for P2P memory
 * @bar_start: Physical start address of BAR
 * @bar_size: Size of the BAR in bytes
 */
struct p2pdma_device {
	struct pci_dev *pdev;
	int minor;
	dev_t devt;
	struct cdev cdev;
	struct device *chardev;
	int bar;
	resource_size_t bar_start;
	resource_size_t bar_size;
};

static long p2pdma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* Character device file operations */
static const struct file_operations p2pdma_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= p2pdma_ioctl,
	.llseek		= noop_llseek,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= p2pdma_ioctl,
#endif
};

struct p2pdma_driver {
    struct class *cls;
    dev_t base_dev;
};
// Global driver state
static struct p2pdma_driver *p2pdma_drv_ctx;

static void p2pdma_ida_free_action(void *data) {
    int idx = (long)data;
    ida_free(&minors_ida, idx);
}

static void p2pdma_cdev_del_action(void *data) {
    struct cdev *cdev = data;
    cdev_del(cdev);
}

static void p2pdma_device_destroy_action(void *data) {
    struct p2pdma_device *p2pdma_dev = data;

	if (p2pdma_drv_ctx && p2pdma_drv_ctx->cls) {
		device_destroy(p2pdma_drv_ctx->cls, p2pdma_dev->devt);
	}
}


static long p2pdma_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct p2pdma_device *p2pdma_dev;
	struct inode *inode;

	if (!filp) {
		pr_err("Invalid file pointer\n");
		return -EINVAL;
	}

	inode = file_inode(filp);
	if (!inode) {
		pr_err("Invalid inode\n");
		return -EINVAL;
	}

	p2pdma_dev = container_of(inode->i_cdev, struct p2pdma_device, cdev);
	if (!p2pdma_dev || !p2pdma_dev->pdev) {
		pr_err("Invalid device context\n");
		return -EINVAL;
	}

	dev_dbg(&p2pdma_dev->pdev->dev, "ioctl cmd=0x%x\n", cmd);

	switch (cmd) {
	case QDMA_P2PDMA_IOCTL_DISABLE:
		pci_p2pmem_publish(p2pdma_dev->pdev, false);
		dev_info(&p2pdma_dev->pdev->dev, "P2P memory disabled\n");
		return 0;

	case QDMA_P2PDMA_IOCTL_ENABLE:
		pci_p2pmem_publish(p2pdma_dev->pdev, true);
		dev_info(&p2pdma_dev->pdev->dev, "P2P memory enabled\n");
		return 0;

	default:
		dev_warn(&p2pdma_dev->pdev->dev, "Unknown ioctl cmd=0x%x\n", cmd);
		return -ENOTTY;
	}
}


static int qdma_p2p_create_chardev(struct pci_dev *pdev, int bar) {
	struct p2pdma_device *p2pdma_dev = NULL;
	int rv;

	p2pdma_dev = devm_kzalloc(&pdev->dev, sizeof(*p2pdma_dev), GFP_KERNEL);
	if (!p2pdma_dev) {
		dev_err(&pdev->dev, "Failed to allocate device context\n");
		return -ENOMEM;
	}

	p2pdma_dev->pdev = pdev;
	p2pdma_dev->bar = bar;
	p2pdma_dev->bar_start = pci_resource_start(pdev, bar);
	p2pdma_dev->bar_size = pci_resource_len(pdev, bar);

	dev_info(&pdev->dev,
		 "BAR%d start address %pa with length %pa bytes\n",
		 bar, &p2pdma_dev->bar_start, &p2pdma_dev->bar_size);

	rv = ida_alloc_range(&minors_ida, QDMA_P2PDMA_MINOR_START, QDMA_P2PDMA_NUM_MINORS - 1, GFP_KERNEL);
	if (rv < 0) {
		dev_err_probe(&pdev->dev, rv, "Failed to allocate minor number");
		return rv;
	}
	p2pdma_dev->minor = rv;
	devm_add_action_or_reset(&pdev->dev, p2pdma_ida_free_action, (void *)(long)p2pdma_dev->minor);

	cdev_init(&p2pdma_dev->cdev, &p2pdma_fops);
	p2pdma_dev->cdev.owner = THIS_MODULE;

	p2pdma_dev->devt = MKDEV(MAJOR(p2pdma_drv_ctx->base_dev), MINOR(p2pdma_drv_ctx->base_dev) + p2pdma_dev->minor);
	rv = cdev_add(&p2pdma_dev->cdev, p2pdma_dev->devt, 1);
	if (rv) {
	dev_err_probe(&pdev->dev, rv, "Failed to add character device");
	    goto err_ida;
	}
	devm_add_action_or_reset(&pdev->dev, p2pdma_cdev_del_action, &p2pdma_dev->cdev);

	p2pdma_dev->chardev = device_create(p2pdma_drv_ctx->cls, &pdev->dev, p2pdma_dev->devt,
					    p2pdma_dev, QDMA_P2PDMA_DRV_BASENAME"%d", p2pdma_dev->minor);
	if (IS_ERR(p2pdma_dev->chardev)) {
		rv = PTR_ERR(p2pdma_dev->chardev);
		dev_err_probe(&pdev->dev, rv, "Failed to create character device");
		goto err_cdev;
	}
	devm_add_action_or_reset(&pdev->dev, p2pdma_device_destroy_action, p2pdma_dev);

	return 0;

err_cdev:
	cdev_del(&p2pdma_dev->cdev);
err_ida:
	ida_free(&minors_ida, p2pdma_dev->minor);
	return rv;
}

/**
 * qdma_p2pdma_init - Initialize the P2PDMA driver subsystem
 *
 * This function must be called once during driver initialization to set up
 * the character device region and device class for P2PDMA devices.
 *
 * Return: 0 on success, negative error code on failure
 */
int qdma_p2pdma_init(void) {
    int rv;

	/* Check if already initialized */
	if (p2pdma_drv_ctx) {
		kfree(p2pdma_drv_ctx);
	}

	/* Allocate global driver context */
	p2pdma_drv_ctx = kzalloc(sizeof(*p2pdma_drv_ctx), GFP_KERNEL);
	if (!p2pdma_drv_ctx) {
		pr_err("Failed to allocate driver context\n");
		return -ENOMEM;
	}

	/* Allocate character device region */
	rv = alloc_chrdev_region(&p2pdma_drv_ctx->base_dev,
				 QDMA_P2PDMA_MINOR_START,
				 QDMA_P2PDMA_NUM_MINORS,
				 QDMA_P2PDMA_DRV_BASENAME);
	if (rv < 0) {
		pr_err("Failed to allocate character device region: %d\n", rv);
		goto err_free_ctx;
	}

	/* Create device class */
	p2pdma_drv_ctx->cls = class_create(QDMA_P2PDMA_DRV_BASENAME);
	if (IS_ERR_OR_NULL(p2pdma_drv_ctx->cls)) {
        pr_err("Failed to create device class: %ld\n", PTR_ERR(p2pdma_drv_ctx->cls));
		rv = -EINVAL;
		goto err_unreg_chrdev;
	}

    pr_info("P2P DMA driver initialized (major=%d)\n", MAJOR(p2pdma_drv_ctx->base_dev));

    return 0;

err_unreg_chrdev:
	unregister_chrdev_region(p2pdma_drv_ctx->base_dev, QDMA_P2PDMA_NUM_MINORS);
err_free_ctx:
	kfree(p2pdma_drv_ctx);
	p2pdma_drv_ctx = NULL;
	return rv;
}

/**
 * qdma_p2pdma_probe - Initialize P2PDMA for a PCI device
 * @pdev: PCI device to initialize
 * @bar: BAR number to use for P2PDMA memory
 *
 * Return: 0 on success, negative error code on failure
 */
int qdma_p2pdma_probe(struct pci_dev *pdev, int bar) {
    unsigned long flags;
    int rv;

	if (!pdev) {
		pr_err("Invalid PCI device\n");
		return -EINVAL;
	}

	if (!p2pdma_drv_ctx) {
		dev_err(&pdev->dev, "P2P DMA driver not initialized\n");
		return -ENODEV;
	}

	/* A tandem/streaming-only design may expose no user-memory BAR.
	 * GPU DMA-BUF import and normal QDMA operation do not require one.
	 */
	if (bar < 0) {
		dev_info(&pdev->dev, "No user BAR; skipping P2PDMA provider\n");
		return 0;
	}

	if (bar >= PCI_STD_NUM_BARS) {
		dev_err(&pdev->dev, "Invalid BAR number: %d\n", bar);
		return -EINVAL;
	}

	// Check if BAR is a memory resource
	flags = pci_resource_flags(pdev, bar);
	if (!(flags & IORESOURCE_MEM) || !pci_resource_len(pdev, bar)) {
		dev_info(&pdev->dev, "BAR%d has no memory resource; skipping P2PDMA provider\n", bar);
		return 0;
	}

	// Register BAR as P2PDMA provider memory. size=0 to use whole BAR.
	dev_info(&pdev->dev, "Registering BAR%d (size %llu bytes) as P2PDMA provider memory.\n", bar, pci_resource_len(pdev, bar));
	rv = pci_p2pdma_add_resource(pdev, bar, 0, 0);
	if (rv) {
		dev_err(&pdev->dev, QDMA_P2PDMA_DRV_BASENAME ": pci_p2pdma_add_resource failed: %d\n", rv);
		return rv;
	}

	pci_p2pmem_publish(pdev, true);

	// Create character device interface
	rv = qdma_p2p_create_chardev(pdev, bar);
	if (rv) {
		dev_err(&pdev->dev, "Failed to create character device: %d\n", rv);
		return rv;
	}

	dev_info(&pdev->dev, QDMA_P2PDMA_DRV_BASENAME ": P2P provider ready on %s, BAR%d %s\n",
		 dev_name(&pdev->dev), bar, (flags & IORESOURCE_PREFETCH) ? ", prefetchable" : "");
	return 0;
}

/**
 * qdma_p2pdma_remove - Remove P2P DMA support for a PCI device
 * @pdev: PCI device to remove
 */
void qdma_p2pdma_remove(struct pci_dev *pdev)
{
	if (!pdev) {
		pr_warn("Invalid PCI device for removal\n");
		return;
	}
	pci_p2pmem_publish(pdev, false);

	dev_info(&pdev->dev, "P2P DMA device removed\n");
}

/**
 * qdma_p2pdma_exit - Cleanup the P2PDMA driver subsystem
 */
void qdma_p2pdma_exit(void)
{
	if (!p2pdma_drv_ctx) {
		pr_info("P2P DMA driver not initialized, nothing to cleanup\n");
		return;
	}

	// Cleanup device class
	if (p2pdma_drv_ctx->cls && !IS_ERR(p2pdma_drv_ctx->cls)) {
	class_destroy(p2pdma_drv_ctx->cls);
	}

	// Cleanup character device region
	unregister_chrdev_region(p2pdma_drv_ctx->base_dev, QDMA_P2PDMA_NUM_MINORS);

	// Cleanup minor ID allocator
	ida_destroy(&minors_ida);

	// Free driver context
	kfree(p2pdma_drv_ctx);
	p2pdma_drv_ctx = NULL;

	pr_info("P2P DMA driver unloaded\n");
}
