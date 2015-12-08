/* -LICENSE-START-
** Copyright (c) 2009-2013 Blackmagic Design
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

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/wait.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/jhash.h>
#include <asm/page.h>
#include <asm/i387.h>
#include <asm/div64.h>
#include <asm/atomic.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/uaccess.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 4, 0)
	#include <asm/fpu-internal.h>
#endif


const unsigned int bmd_page_shift = PAGE_SHIFT;
const unsigned int bmd_page_size = PAGE_SIZE;
const int bmd_verify_read = VERIFY_READ;
const int bmd_verify_write = VERIFY_WRITE;

const char *DL_KERN_INFO = KERN_INFO;
const char *DL_KERN_WARNING = KERN_WARNING;
const char *DL_KERN_ERR = KERN_ERR;

#include "blackmagic_lib.h"

static struct kmem_cache *__dl_wait_queue_cache = NULL;

struct dl_wait_queue_head_t
{
	struct list_head entry;
	wait_queue_head_t wqh;
	atomic_t state;
	atomic_t sleepers;
	void *private_data;
};

struct dl_spinlock_t
{
	spinlock_t lock;
};

inline int dl_flush_cache_all(void)
{
	return 0;
}

inline void *dl_kzalloc(unsigned int size)
{
	return kzalloc(size, in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
}

inline void *dl_kmalloc(unsigned int size)
{
	return kmalloc(size, in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
}

inline void dl_kfree(void *ptr)
{
    kfree(ptr);
}

inline void *dl_vmalloc(unsigned int size)
{
	return vmalloc(size);
}

static struct work_struct vmallocWork;
static DEFINE_SPINLOCK(vmallocLock);
static LIST_HEAD(vmallocList);

struct vmallocWorkEntry
{
	void *ptr;
	struct list_head list;
	unsigned int operation;
};
#define VMALLOC_OPERATION_VFREE  1
#define VMALLOC_OPERATION_VUNMAP 2

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
static void do_vmalloc_work(struct work_struct * data)
#else
static void do_vmalloc_work(void * data)
#endif
{
	struct vmallocWorkEntry *e;
	LIST_HEAD(list);

	spin_lock_bh(&vmallocLock);
	list_splice_init(&vmallocList, &list);
	spin_unlock_bh(&vmallocLock);

	while (!list_empty(&list))
	{
		e = list_entry(list.next, struct vmallocWorkEntry, list);
		switch (e->operation)
		{
			case VMALLOC_OPERATION_VFREE:
				vfree(e->ptr);
				break;
			case VMALLOC_OPERATION_VUNMAP:
				vunmap(e->ptr);
				break;
		}
		list_del(list.next);
		kfree(e);
	}
}

/*
 * vfree can't be called from in an interrupt, so if we're in an interrupt
 * (or tasklet), queue the deletion on the system work queue
 */
inline void dl_vfree(void *ptr)
{
	struct vmallocWorkEntry *work;
	if (!in_interrupt())
	{
		vfree(ptr);
	}
	else
	{
		spin_lock_bh(&vmallocLock);
			if (list_empty(&vmallocList))
			{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
				INIT_WORK(&vmallocWork, do_vmalloc_work);
#else
				INIT_WORK(&vmallocWork, do_vmalloc_work, NULL);
#endif
				schedule_work(&vmallocWork);
			}

			work = kmalloc(sizeof(struct vmallocWorkEntry), GFP_ATOMIC);
			BUG_ON(!work);

			INIT_LIST_HEAD(&work->list);
			work->operation = VMALLOC_OPERATION_VFREE;
			work->ptr = ptr;
			list_add(&work->list, &vmallocList);

		spin_unlock_bh(&vmallocLock);
	}
}

inline struct dl_spinlock_t *dl_alloc_spinlock(void)
{
	struct dl_spinlock_t *lock;
	lock = (struct dl_spinlock_t *)dl_kmalloc(sizeof(struct dl_spinlock_t));
	
	if (lock)
		spin_lock_init(&lock->lock);
	return lock;
}

inline void dl_free_spinlock(struct dl_spinlock_t *lock)
{
	dl_kfree(lock);
}

inline void dl_spin_lock_irqsave(struct dl_spinlock_t *lock, unsigned long *iflags)
{
	spin_lock_irqsave(&lock->lock, *iflags);
}

inline void dl_spin_unlock_irqrestore(struct dl_spinlock_t *lock, unsigned long iflags)
{
	spin_unlock_irqrestore(&lock->lock, iflags);
}

inline void *dl_vmap(void *page_array, unsigned long page_num)
{
	return vmap((struct page **)page_array, page_num, VM_MAP, PAGE_SHARED);
}

/*
 * vunmap can't be called from in an interrupt, so if we're in an interrupt
 * (or tasklet), queue the unmap on the system work queue
 */
inline void dl_vunmap(void *address)
{
	struct vmallocWorkEntry *work;
	if (!in_interrupt())
	{
		vunmap(address);
	}
	else
	{
		spin_lock_bh(&vmallocLock);
			if (list_empty(&vmallocList))
			{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20)
				INIT_WORK(&vmallocWork, do_vmalloc_work);
#else
				INIT_WORK(&vmallocWork, do_vmalloc_work, NULL);
#endif
				schedule_work(&vmallocWork);
			}

			work = kmalloc(sizeof(struct vmallocWorkEntry), GFP_ATOMIC);
			BUG_ON(!work);

			INIT_LIST_HEAD(&work->list);
			work->operation = VMALLOC_OPERATION_VUNMAP;
			work->ptr = address;
			list_add(&work->list, &vmallocList);

		spin_unlock_bh(&vmallocLock);
	}
}

inline unsigned int dl_ioread32(volatile void *addr)
{
	return ioread32((void *)addr);
}

inline void dl_iowrite32(unsigned int value, volatile void *addr)
{
   	iowrite32(value, (void *)addr);
}

inline int dl_compare_and_swap(volatile unsigned int *v, int old, int new)
{
	int prev;
	prev = cmpxchg((volatile int *)v, old, new);
	if (prev == old)
		return 1;
	return 0;
}

inline unsigned int dl_bit_or_atomic(unsigned int mask, unsigned int *value)
{
	unsigned int old;
	unsigned int new;
	do {
		old = *value;
		new = (old | mask);
	} while (!dl_compare_and_swap(value, old, new));
   
	return old;
}

inline void *dl_memset(void *a, int c, unsigned int n)
{
	return memset(a, c, n);
}

inline void *dl_memcpy(void *s1, const void *s2, unsigned int n)
{
	return memcpy(s1, s2, n);
}

inline int dl_memcmp(const void *s1, const void *s2, unsigned int n)
{
	return memcmp(s1, s2, n);
}

inline unsigned int dl_strlen(const char *s)
{
	return strlen(s);
}

inline char *dl_strncpy(char *s1, const char *s2, unsigned int n)
{
	return strncpy(s1, s2, n);
}

int dl_printk(const char *fmt, ...)
{
	va_list args;
	int r;
    
	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);
    
	return r;
}

inline unsigned long long
dl_uptime(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	return get_jiffies_64() - INITIAL_JIFFIES;
#else
	struct timespec ts;
	getrawmonotonic(&ts);
	return ((ts.tv_sec * 1000000000ULL) + ts.tv_nsec);
#endif
}

inline unsigned long long dl_get_time_us()
{
	struct timeval t;
	do_gettimeofday(&t);
	return (t.tv_sec * USEC_PER_SEC + t.tv_usec);
}

inline long long
dl_to_nano_secs(unsigned long long time)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	return ((1000000000ULL / HZ) * time);
#else
	return time;
#endif
}

inline unsigned long
dl_jiffies_in_unit(long value, int unit)
{
	if (unit == kMillisecondScale)
		return msecs_to_jiffies(value);
	
	BUG_ON(unit != kMillisecondScale);
	return 0;
}

/*
 *
 * Functions for accessing PCI configuration space.
 *
 */
inline unsigned int
dl_pci_read_config_dword(void *pci_dev, int offset)
{
	uint32_t val;
	pci_read_config_dword((struct pci_dev *)pci_dev, offset, &val);
	return val;
}

inline unsigned short int
dl_pci_read_config_word(void *pci_dev, int offset)
{
	unsigned short int val;
	pci_read_config_word((struct pci_dev *)pci_dev, offset, &val);
	return val;
}

inline unsigned char
dl_pci_read_config_byte(void *pci_dev, int offset)
{
	uint8_t val;
	pci_read_config_byte((struct pci_dev *)pci_dev, offset, &val);
	return val;
}

inline int
dl_pci_write_config_dword(void *pci_dev, int offset, unsigned int val)
{
	return pci_write_config_dword((struct pci_dev *)pci_dev, offset, val);
}

inline int
dl_pci_write_config_word(void *pci_dev, int offset, unsigned short int val)
{
	return pci_write_config_word((struct pci_dev *)pci_dev, offset, val);
}

inline int
dl_pci_write_config_byte(void *pci_dev, int offset, unsigned char val)
{
	return pci_write_config_byte((struct pci_dev *)pci_dev, offset, val);
}

inline void *
dl_pci_map_bar(void *pci_dev, int bar)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	dma_addr_t base;
    
	base = pci_resource_start(dev, bar);
	return ioremap_nocache(base, pci_resource_len(dev, bar));
}

inline void
dl_pci_unmap_bar(void *address)
{
	iounmap(address);
}

inline unsigned short
dl_pci_get_bus_num(void *pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	return dev->bus->number;
}

inline unsigned short
dl_pci_get_device_num(void *pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	return dev->device;
}

inline unsigned short
dl_pci_get_func_num(void *pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	return PCI_FUNC(dev->devfn);
}

inline unsigned short
dl_pci_get_slot_num(void *pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	return PCI_SLOT(dev->devfn);
}

inline void
dl_pci_set_bus_master(void *pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
	pci_set_master(dev);
	return; 
}

bool dl_pci_supports_msi(void* pci_dev)
{
	struct pci_dev *dev = (struct pci_dev *)pci_dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
	return pci_find_capability(dev, PCI_CAP_ID_MSI) != 0;
#else
	return dev->msi_cap != 0;
#endif
}

inline void *
dl_pci_get_parent_pci_dev(void *pci_dev)
{
	struct pci_dev *parent_pci_dev = NULL;
	struct pci_dev *dev = (struct pci_dev *) pci_dev;

	if (dev && dev->bus)
		parent_pci_dev = dev->bus->self;

	return parent_pci_dev;
}

inline void *
dl_alloc_semaphore(void)
{
	struct semaphore *sem = kmalloc(sizeof(struct semaphore), GFP_KERNEL);
	if (!sem)
		return NULL;
	sema_init(sem, 1);
	return sem;
}

inline void
dl_sema_down(void *ptr)
{
	struct semaphore *sem = (struct semaphore *)ptr;
	down(sem);
}

inline int
dl_sema_down_trylock(void *ptr)
{
	struct semaphore *sem = (struct semaphore *)ptr;
	return (down_trylock(sem) == 0);
}

inline int
dl_sema_down_timeout(void *mutex, unsigned long timeout, unsigned int *cond)
{
	unsigned int it = jiffies_to_msecs(timeout)/100;
	unsigned int orig_cond = *cond;
	int res = THREAD_TIMED_OUT;

	up(mutex);
	while (--it)
	{
		if (msleep_interruptible(100))
		{
			res = THREAD_INTERRUPTED;
			break;
		}
		if (*cond != orig_cond)
		{
			res = THREAD_AWAKENED;
			break;
		}
	}
	down(mutex);

	return res;
}

inline void
dl_sema_up(void *ptr)
{
	struct semaphore *sem = (struct semaphore *)ptr;
	up(sem);
}

inline void
dl_sema_free(void *ptr)
{
	kfree(ptr);
}

struct dl_thread_wrapper_struct
{
	thread_continue_t func;
	void *param;
};

int dl_thread_wrapper(void *data)
{
	struct dl_thread_wrapper_struct *tws = data;
	tws->func(tws->param, 0);
	kfree(tws);
	return 0;
}

int dl_kernel_thread_start(thread_continue_t func, void *param, thread_t *id)
{
	struct dl_thread_wrapper_struct *tws;
	struct task_struct *tsk;
	
	tws = kzalloc(sizeof(struct dl_thread_wrapper_struct), GFP_KERNEL);
	if (!tws)
		return 1;

	tws->func = func;
	tws->param = param;
	
	tsk = kthread_run(dl_thread_wrapper, tws, "blackmagicd");
	if (IS_ERR(tsk))
	{
		if (id)
		 *id = NULL;
		kfree(tws);	
		return 1;
	}
	*id = (void*)tsk;
	return 0;
}

inline void dl_udelay(unsigned long usecs)
{
	udelay(usecs);
}

inline void dl_msleep(unsigned long msecs)
{
	msleep(msecs);
}

unsigned long long dl_div64(unsigned long long a, unsigned long long b)
{
	do_div(a, b);
    return (unsigned long long)a;
}

unsigned long long dl_mod64(unsigned long long a, unsigned long long b)
{
    return do_div(a, b);
}

inline unsigned long
__dl_copy_from_user(void *to, const void *from, unsigned long n)
{
	return __copy_from_user(to, from, n);
}

inline unsigned long 
__dl_copy_to_user(void *to, const void *from, unsigned long n)
{
	return __copy_to_user(to, from, n);
}

inline int 
dl_access_ok(int type, void *addr, unsigned long size)
{
	return access_ok(type, addr, size);
}

void *
dl_get_current()
{
	return current;
}

void *
dl_get_user_pages(void *task_ptr, void *ptr, unsigned long size, unsigned long *nr_pages, int write)
{
	int ret;
	struct task_struct *current_task = task_ptr;
	struct page **pages;
	unsigned long first, last;
	
	if (!current_task)
		return NULL;

	first = ((unsigned long)ptr) >> PAGE_SHIFT;
	last = ((unsigned long)ptr + size - 1) >> PAGE_SHIFT;
	*nr_pages = last - first + 1;

	pages = kmalloc(*nr_pages * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	if (write == DL_DMA_BIDIRECTIONAL || write == DL_DMA_FROM_DEVICE)
		write = 1;
	else
		write = 0;
	
	down_read(&current_task->mm->mmap_sem);
	ret = get_user_pages(current_task, 
	                     current_task->mm, 
						 (unsigned long)ptr & PAGE_MASK, 
						 *nr_pages, write, 0, pages, NULL);
	up_read(&current_task->mm->mmap_sem);
	
	if (ret < *nr_pages)
	{
		dl_unmap_user_pages(pages, ret, 0); 
		return NULL;
	}

	return pages;
}

void
dl_unmap_user_pages(void *ptr, unsigned long nr_pages, int flag_dirty)
{
	unsigned long i;
	struct page *p;
	struct page **pages = (struct page**)ptr;
	
	for (i = 0; i < nr_pages; i++)
	{
		p = pages[i];
	    if (p == NULL)
            continue;

		if (flag_dirty)
			SetPageDirty(p);
		page_cache_release(p);
	}
	kfree(ptr);
}

inline struct dl_wait_queue_head_t *dl_alloc_waitqueue(void)
{
	struct dl_wait_queue_head_t *queue;
	
	if (__dl_wait_queue_cache == NULL)
	{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23)
		__dl_wait_queue_cache = 
			kmem_cache_create("dl_wait_queue_head_t", 
			                  sizeof(struct dl_wait_queue_head_t), 0, 0, NULL); 
#else
		__dl_wait_queue_cache = 
			kmem_cache_create("dl_wait_queue_head_t", 
			                  sizeof(struct dl_wait_queue_head_t), 0, 0, NULL, NULL); 
#endif
		if (!__dl_wait_queue_cache)
			return NULL;
	}
	
	queue = (struct dl_wait_queue_head_t*) kmem_cache_alloc(__dl_wait_queue_cache, GFP_KERNEL);
	
	if (queue)
	{
		init_waitqueue_head(&queue->wqh);
		INIT_LIST_HEAD(&queue->entry);
		atomic_set(&queue->state, 0);
		atomic_set(&queue->sleepers, 0);
		queue->private_data = NULL;
	}
	return queue;
}

void dl_free_waitqueue(struct dl_wait_queue_head_t *queue)
{
	kmem_cache_free(__dl_wait_queue_cache, queue);
}

void *dl_get_wait_queue_ptr(struct dl_wait_queue_head_t *queue)
{
	return &queue->wqh;
}

inline void dl_set_wait_queue_event(struct dl_wait_queue_head_t *queue)
{
	atomic_set(&queue->state, 1);
	wake_up_interruptible(&queue->wqh);
}

inline void dl_clear_wait_queue_event(struct dl_wait_queue_head_t *queue)
{
	atomic_set(&queue->state, 0);	
}

inline int dl_get_wait_queue_event_state(struct dl_wait_queue_head_t *queue)
{
	return atomic_read(&queue->state);	
}

void dl_destroy_wait_queue_cache(void)
{
	if (__dl_wait_queue_cache != NULL)
	{
		kmem_cache_destroy(__dl_wait_queue_cache);
		__dl_wait_queue_cache = NULL;
	}
}

unsigned int
dl_poll_wait(void *filp, struct dl_wait_queue_head_t *queue, void *wait, int write)
{
	unsigned int mask = 0;

	poll_wait((struct file *)filp, &queue->wqh, wait);

	if ((atomic_read(&queue->state) == 1) && write)
		return mask |= POLLOUT | POLLWRNORM;
	else if ((atomic_read(&queue->state) == 1) && !write)
		return mask |= POLLIN | POLLRDNORM;

	return mask;
}

inline void
dl_kernel_fpu_begin()
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
	kernel_fpu_begin();
#else
	struct thread_info *thread;
	thread = current_thread_info();
	
	preempt_disable();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 8)
	if (__thread_has_fpu(current))
#else
	#ifdef TS_USEDFPU
		if (thread->status & TS_USEDFPU)
	#else
		if (__thread_has_fpu(current))
	#endif
#endif
	{

#if defined(__x86_64__)
		#define FX_SAVE_INSTR	"rex64 ; fxsave %0 ; fnclex"
#elif defined(__i386__)
		#define FX_SAVE_INSTR	"fxsave %0; fnclex"
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
		asm volatile(FX_SAVE_INSTR : "=m" (thread->task->thread.fpu.state->fxsave));
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 26)
		asm volatile(FX_SAVE_INSTR : "=m" (thread->task->thread.xstate->fxsave));
#else
		asm volatile(FX_SAVE_INSTR : "=m" (thread->task->thread.i387.fxsave));
#endif
	}
	else
		clts();
#endif
}

inline void dl_kernel_fpu_end(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
	kernel_fpu_end();
#else
	stts();
	preempt_enable();
#endif
}

void dl_backtrace(void)
{
	dump_stack();
}

unsigned int dl_hash_string(const char *str, unsigned int bits)
{
	return jhash(str, strlen(str), 0) >> (32 - bits);
}

int dl_strcmp(const char* str1, const char *str2)
{
	return strcmp(str1, str2);
}

void dl_schedule()
{
	schedule();
}
