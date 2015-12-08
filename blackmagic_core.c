/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/version.h>

#include "blackmagic_core.h"

unsigned long blackmagic_flags = 0;
module_param(blackmagic_flags, ulong, S_IRUGO | S_IWUSR);

static struct pci_device_id blackmagic_ids[] = {
	{ PCI_DEVICE(0xbdbd, 0xa10b) },
	{ PCI_DEVICE(0xbdbd, 0xa10c) },
	{ PCI_DEVICE(0xbdbd, 0xa10e) },
	{ PCI_DEVICE(0xbdbd, 0xa10f) },
	{ PCI_DEVICE(0xbdbd, 0xa113) },
	{ PCI_DEVICE(0xbdbd, 0xa114) },
	{ PCI_DEVICE(0xbdbd, 0xa115) },
	{ PCI_DEVICE(0xbdbd, 0xa116) },
	{ PCI_DEVICE(0xbdbd, 0xa117) },
	{ PCI_DEVICE(0xbdbd, 0xa118) },
	{ PCI_DEVICE(0xbdbd, 0xa119) },
	{ PCI_DEVICE(0xbdbd, 0xa11a) },
	{ PCI_DEVICE(0xbdbd, 0xa11b) },
	{ PCI_DEVICE(0xbdbd, 0xa11c) },
	{ PCI_DEVICE(0xbdbd, 0xa11d) },
	{ PCI_DEVICE(0xbdbd, 0xa11e) },
	{ PCI_DEVICE(0xbdbd, 0xa120) },
	{ PCI_DEVICE(0xbdbd, 0xa121) },
	{ PCI_DEVICE(0xbdbd, 0xa114) },
	{ PCI_DEVICE(0xbdbd, 0xa117) },
	{ PCI_DEVICE(0xbdbd, 0xa12f) },
	/* Thunderbolt */
	{ PCI_DEVICE(0xbdbd, 0xa123) },
	{ PCI_DEVICE(0xbdbd, 0xa124) },
	{ PCI_DEVICE(0xbdbd, 0xa126) },
	{ PCI_DEVICE(0xbdbd, 0xa127) },
	{ PCI_DEVICE(0xbdbd, 0xa129) },
	{ PCI_DEVICE(0xbdbd, 0xa12a) },
	{ 0 }
};

extern int __init blackmagic_serial_init(void);
extern int __init blackmagic_serial_exit(void);
extern int blackmagic_serial_probe(struct blackmagic_device *, struct device *dev);
extern void blackmagic_serial_remove(struct blackmagic_device *);

#ifdef __i386__
	// 32-bit systems may not have a 64-bit cmpxchg function, so limit the
	// supported number of ids to 32.
	typedef uint32_t device_mask_id_t;
#else
	typedef uint64_t device_mask_id_t;
#endif

static device_mask_id_t blackmagic_device_ids = 0;
static LIST_HEAD(blackmagic_devices);
static DEFINE_SPINLOCK(blackmagic_devices_lock);

struct blackmagic_device *
blackmagic_find_device_by_minor(int minor)
{
	struct blackmagic_device *dev = NULL;
	unsigned long flags;

	spin_lock_irqsave(&blackmagic_devices_lock, flags);

	list_for_each_entry(dev, &blackmagic_devices, entry)
	{
		if (dev->mdev.minor == minor)
			break;
	}

	spin_unlock_irqrestore(&blackmagic_devices_lock, flags);

	return dev;
}

struct blackmagic_device *
blackmagic_find_device_by_id(int id)
{
	struct blackmagic_device *dev = NULL;
	unsigned long flags;

	spin_lock_irqsave(&blackmagic_devices_lock, flags);

	list_for_each_entry(dev, &blackmagic_devices, entry)
	{
		if (dev->id == id)
			break;
	}

	spin_unlock_irqrestore(&blackmagic_devices_lock, flags);

	return dev;
}

struct blackmagic_device *blackmagic_find_device_by_ptr(void *ptr)
{
	struct blackmagic_device *dev = NULL;
	unsigned long flags;

	spin_lock_irqsave(&blackmagic_devices_lock, flags);

	list_for_each_entry(dev, &blackmagic_devices, entry)
	{
		if (dev->driver == ptr)
			break;
	}

	spin_unlock_irqrestore(&blackmagic_devices_lock, flags);

	return dev;
}

/*
 * Outer interrupt service routine.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static irqreturn_t blackmagic_isr(int irq, void *dev, struct pt_regs *regs)
#else
static irqreturn_t blackmagic_isr(int irq, void *dev)
#endif
{
    unsigned int status;
	struct blackmagic_device *ddev = (struct blackmagic_device *)dev;
	
	if (!ddev)
		return IRQ_NONE;
	
	status = dl_interrupt_handler(ddev->driver);
	
	if (status & DL_INTERRUPT_SCHED_TASKLET)
    {
		tasklet_schedule(&ddev->tasklet);
		return IRQ_HANDLED;
	}
	else if (status & DL_INTERRUPT_HANDLED)
		return IRQ_HANDLED;
	
	return IRQ_NONE;
}

/*
 * Main entry point for when an application/API opens a device.
 */
static int blackmagic_open(struct inode *inode, struct file *filp)
{
	struct blackmagic_device *ddev;
	void *uclient;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	ddev = blackmagic_find_device_by_minor(iminor(file_inode(filp)));
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
	//NOTE: inode is/may be NULL on >=2.6.36
	ddev = blackmagic_find_device_by_minor(iminor(filp->f_dentry->d_inode));
#else
	ddev = blackmagic_find_device_by_minor(iminor(inode));
#endif

	if (!ddev)
		return -ENODEV;
	
	if (!atomic_read(&ddev->ready))
		return -EBUSY;

	uclient = dl_create_and_init_user_client(ddev->driver, current);
	if (IS_ERR(uclient))
		return PTR_ERR(uclient);
	filp->private_data = uclient;

	return 0;
}

static int blackmagic_release(struct inode *inode, struct file *filp)
{
	struct blackmagic_device *ddev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	ddev = blackmagic_find_device_by_minor(iminor(file_inode(filp)));
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
	ddev = blackmagic_find_device_by_minor(iminor(filp->f_dentry->d_inode));
#else
	ddev = blackmagic_find_device_by_minor(iminor(inode));
#endif

	if (!ddev)
		return -ENODEV;

	/* try to close the serial port in case it was opened in IOCTL mode 
	 * (does nothing if the serial port was closed or opened through TTY layer)
	 */
	blackmagic_serial_close_ioctl(ddev->driver);

	/* detach from the driver, and free the user client class */
	dl_release_user_client(filp->private_data);
	return 0;
}

#ifdef HAVE_UNLOCKED_IOCTL
static long
blackmagic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
#else
static int
blackmagic_ioctl(struct inode *inode, struct file *filp,
               unsigned int cmd, unsigned long arg)
#endif
{
	struct blackmagic_device *ddev;
#if HAVE_UNLOCKED_IOCTL
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	ddev = blackmagic_find_device_by_minor(iminor(file_inode(filp)));
#else
	ddev = blackmagic_find_device_by_minor(iminor(filp->f_dentry->d_inode));
#endif
#else
	ddev = blackmagic_find_device_by_minor(iminor(inode));
#endif
	
	if (!ddev)
		return -ENODEV;
	
	if (!filp->private_data)
		return -ENODEV;
	
	return blackmagic_ioctl_private(ddev->driver, filp->private_data, cmd, arg);
}

/*
 * Implements select/poll system call. POLLIN is used to signal to precense
 * of video input frames, and POLLOUT output.
 */
static unsigned int
blackmagic_poll(struct file *filp, poll_table *wait)
{
	return dl_driver_do_poll(filp->private_data, filp, wait);
}

static int blackmagic_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int r;
	void* buffer;
	unsigned long size;

	if (!filp->private_data)
		return -ENODEV;

	r = dl_mmap_buffer(filp->private_data, vma->vm_pgoff, &buffer, &size);
	if (r < 0)
		return r;

	return remap_pfn_range(vma, vma->vm_start, __pa(buffer) >> PAGE_SHIFT, size, vma->vm_page_prot);
}

struct file_operations blackmagic_fops = {
	.owner   = THIS_MODULE,
	.open = blackmagic_open,
	.release = blackmagic_release,
#ifdef HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = blackmagic_ioctl,
#else
	.ioctl = blackmagic_ioctl,
#endif
#ifdef HAVE_COMPAT_IOCTL
	.compat_ioctl = blackmagic_ioctl,
#endif
	.poll = blackmagic_poll,
	.mmap = blackmagic_mmap,
};

#define NAME_MAX_LEN	20

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
static void do_bh_work(struct work_struct *work)
{
	struct blackmagic_device *dev = container_of(work, struct blackmagic_device, work);
#else
static void do_bh_work(void *data)
{
	struct blackmagic_device *dev = (struct blackmagic_device*)data;
#endif
	dl_bh_work_handler(dev->driver);
}

static void blackmagic_tasklet_handler(unsigned long data)
{
	unsigned int status;
	struct blackmagic_device *dev = (struct blackmagic_device *)data;

	status = dl_tasklet_handler(dev->driver);
	if (status & DL_INTERRUPT_SCHED_WORK)
		schedule_work(&dev->work);
}

static int blackmagic_alloc_id(void)
{
	int id;
	device_mask_id_t mask;
	device_mask_id_t old_id_map;

	for (id = 0; id < sizeof(device_mask_id_t) * 8; /* nothing */)
	{
		mask = (1UL << id);

		old_id_map = blackmagic_device_ids;
		if (!(old_id_map & mask))
		{
			if (cmpxchg(&blackmagic_device_ids, old_id_map, old_id_map | mask) == old_id_map)
				return id;
		}
		else
		{
			++id;
		}
	}

	return -1;
}

static void blackmagic_release_id(int id)
{
	device_mask_id_t old_id_map;

	do
	{
		old_id_map = blackmagic_device_ids;

		if (cmpxchg(&blackmagic_device_ids, old_id_map, old_id_map & ~(1UL << id)) == old_id_map)
			break;
	}
	while (true);
}

struct blackmagic_device *
blackmagic_create_device(struct pci_dev *pdev)
{
	struct blackmagic_device *ddev = NULL;
	char *name = NULL;
	
	ddev = kzalloc(sizeof(struct blackmagic_device), GFP_KERNEL);
	if (!ddev) 
		return NULL;

	ddev->id = -1;
	
	name = kzalloc(NAME_MAX_LEN, GFP_KERNEL);
	if (!name)
		goto fail;

	ddev->id = blackmagic_alloc_id();
	if (ddev->id < 0)
		goto fail;

	snprintf(name, NAME_MAX_LEN, "blackmagic!dv%d", ddev->id);
	
	INIT_LIST_HEAD(&ddev->entry);
	atomic_set(&ddev->ready, 0);
	ddev->flags = 0;
	ddev->mdev.minor = MISC_DYNAMIC_MINOR;
	ddev->mdev.name = name;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
	ddev->mdev.parent = &pdev->dev;
#endif
	ddev->mdev.fops = &blackmagic_fops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	ddev->mdev.mode = 0666;
#endif
	
	if (misc_register(&ddev->mdev) != 0)
		goto fail;
	
	pci_set_drvdata(pdev, ddev);
	ddev->pdev = pci_dev_get(pdev);

	// Prepare bh handlers
	tasklet_init(&ddev->tasklet, blackmagic_tasklet_handler, (unsigned long)ddev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
	INIT_WORK(&ddev->work, do_bh_work);
#else
	INIT_WORK(&ddev->work, do_bh_work, ddev);
#endif

	spin_lock(&blackmagic_devices_lock);
	list_add_tail(&ddev->entry, &blackmagic_devices);
	spin_unlock(&blackmagic_devices_lock);
	return ddev;

fail:
	if (name)
		kfree(name);
	if (ddev->pdev)
		pci_dev_put(ddev->pdev);
	if (ddev->id >= 0)
		blackmagic_release_id(ddev->id);
	if (ddev)
		kfree(ddev);
	return NULL;
}

void
blackmagic_destroy_device(struct blackmagic_device *ddev)
{
	// Stop bh handlers
	flush_scheduled_work(); // Should really use cancel_work_sync, but it's GPL
	tasklet_kill(&ddev->tasklet);

	misc_deregister(&ddev->mdev);

	spin_lock(&blackmagic_devices_lock);
	list_del(&ddev->entry);
	spin_unlock(&blackmagic_devices_lock);
	
	if (ddev->mdev.name)
		kfree(ddev->mdev.name);
	
	pci_dev_put(ddev->pdev);
	pci_set_drvdata(ddev->pdev, NULL);
	ddev->pdev = NULL;

	blackmagic_release_id(ddev->id);

	kfree(ddev);
}

void dl_free_driver(void *driver)
{
	struct blackmagic_device *dev = blackmagic_find_device_by_ptr(driver);
	if (dev)
		blackmagic_destroy_device(dev);
}

bool dl_pci_start(void* pci_dev)
{
	struct pci_dev* pdev = (struct pci_dev*)pci_dev;

	if (pci_enable_device(pdev) < 0)
		return false;

	pci_set_master(pdev);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
	#define BMD_DMA_64_MASK		DMA_BIT_MASK(64)
	#define BMD_DMA_32_MASK		DMA_BIT_MASK(32)
#else
	#define BMD_DMA_64_MASK		DMA_64BIT_MASK
	#define BMD_DMA_32_MASK		DMA_32BIT_MASK
#endif

	if (pci_set_dma_mask(pdev, BMD_DMA_64_MASK) < 0)
	{
		if (pci_set_dma_mask(pdev, BMD_DMA_32_MASK) < 0)
			goto fail;
	}

	return true;

fail:
	pci_disable_device(pdev);
	return false;
}

void dl_pci_stop(void* pci_dev)
{
	struct pci_dev* pdev = (struct pci_dev*)pci_dev;
	pci_disable_device(pdev);
}

bool dl_pci_register_interrupt(void* pci_dev, int source)
{
	struct pci_dev* pdev = (struct pci_dev*)pci_dev;
	struct blackmagic_device* ddev = (struct blackmagic_device*)pci_get_drvdata(pdev);
	unsigned long flags = 0;

	if (source == 1)
		pci_enable_msi(pdev);

	if (!pdev->msi_enabled)
		flags |= IRQF_SHARED;

	if (request_irq(pdev->irq, blackmagic_isr, flags, ddev->mdev.name, ddev) < 0)
	{
		if (pdev->msi_enabled)
			pci_disable_msi(pdev);
		return false;
	}

	return true;
}

void dl_pci_unregister_interrupt(void* pci_dev)
{
	struct pci_dev* pdev = (struct pci_dev*)pci_dev;
	struct blackmagic_device* ddev = (struct blackmagic_device*)pci_get_drvdata(pdev);

	free_irq(pdev->irq, ddev);

	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
}

/*
 * Main entry point for when a PCI device is detected on a BUS.
 * (i.e when kernel boots, when driver is inserted with modprobe/insmod)
 * - allocate blackmagic_device structure
 * - register character device for user-space communication
 * - register IRQ and handler
 * - enable PCI device, and set DMA mask etc.
 */
static int
blackmagic_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct blackmagic_device *ddev;

	ddev = blackmagic_create_device(pdev);
	if (!ddev)
		return -ENOMEM;

	ddev->driver = dl_alloc_driver();
	if (!ddev->driver)
	{
		// If dl_alloc_driver succeeded, blackmagic_destroy_device is called through dl_free_driver
		blackmagic_destroy_device(ddev);
		return -ENODEV;
	}

	if (dl_start_driver(ddev->driver, ddev, pdev, &ddev->flags) < 0)
		return -ENODEV;

	if (ddev->flags & BLACKMAGIC_DEV_HAS_SERIAL)
	{
		if (blackmagic_serial_probe(ddev, &pdev->dev))
			return -ENODEV;
	}
	
	dl_info("Successfully loaded device \"%s\" [pci@%04x:%02x:%02x.%d]\n",
		ddev->mdev.name,
		pci_domain_nr(ddev->pdev->bus),
		ddev->pdev->bus->number,
		PCI_SLOT(ddev->pdev->devfn),
		PCI_FUNC(ddev->pdev->devfn));
	
	atomic_inc(&ddev->ready);
	/* increment usage count (not safe to unload this driver yet) */
	return 0; /* Yay */
}

static int blackmagic_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct blackmagic_device *ddev = (struct blackmagic_device *)
		pci_get_drvdata(pdev);

	blackmagic_suspend_driver(ddev->driver);

	return 0;
}

static void blackmagic_shutdown(struct pci_dev *pdev)
{
	struct blackmagic_device *ddev = (struct blackmagic_device *)
		pci_get_drvdata(pdev);

	blackmagic_suspend_driver(ddev->driver);
}

static int blackmagic_resume(struct pci_dev *pdev)
{
	struct blackmagic_device *ddev = (struct blackmagic_device *)
		pci_get_drvdata(pdev);

	blackmagic_resume_driver(ddev->driver);
	
	return 0;
}

/*
 * Called when driver is removed from the system, e.g rmmod. This is
 * called once for each device installed in the system.
 */
static void blackmagic_remove(struct pci_dev *pdev)
{
	struct blackmagic_device *ddev = (struct blackmagic_device *)
		pci_get_drvdata(pdev);
	
	dl_info("Shutting down device \"%s\" [pci@%04x:%02x:%02x.%d]\n",
		ddev->mdev.name,
		pci_domain_nr(pdev->bus),
		pdev->bus->number,
		PCI_SLOT(pdev->devfn),
		PCI_FUNC(pdev->devfn));

	if (ddev->flags & BLACKMAGIC_DEV_HAS_SERIAL)
		blackmagic_serial_remove(ddev);

	dl_shutdown_driver(ddev->driver);
	// ddev may be deleted at this point
}

static struct pci_driver pci_driver = {
	.name = "blackmagic_driver",
	.id_table = blackmagic_ids,
	.probe = blackmagic_probe,
	.remove = blackmagic_remove,
	.shutdown = blackmagic_shutdown,
	.suspend = blackmagic_suspend,
	.resume = blackmagic_resume,
};



static int __init pci_blackmagic_init(void)
{
	int ret;

	blackmagic_lib_init();
    
	ret = blackmagic_serial_init();
	if (ret)
		return ret;
    
    dl_info("Loading driver (version: 10.5a17)\n");
	return pci_register_driver(&pci_driver);
}

static void __exit pci_blackmagic_exit(void)
{
	pci_unregister_driver(&pci_driver);
	dl_destroy_wait_queue_cache();
	blackmagic_serial_exit();
	blackmagic_lib_destroy();
}

MODULE_AUTHOR("Blackmagic Design Inc. <developer@blackmagicdesign.com>");
MODULE_DESCRIPTION("Blackmagic Design blackmagic driver");
MODULE_VERSION("10.5a17");
MODULE_LICENSE("Proprietary");
MODULE_DEVICE_TABLE(pci, blackmagic_ids);

module_init(pci_blackmagic_init);
module_exit(pci_blackmagic_exit);