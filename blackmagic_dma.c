/* -LICENSE-START-
** Copyright (c) 2011 Blackmagic Design
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
#include <asm/page.h>
#include <asm/i387.h>
#include <asm/div64.h>
#include <asm/atomic.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/uaccess.h>

#include "blackmagic_lib.h"

struct dl_dma_entry
{
	dma_addr_t			dma_addr;
};

struct dl_dma_list
{
	struct pci_dev*		 pdev;	
	union 
	{
		unsigned int	 num_pages;
		unsigned int	 size;
	};
	uint8_t				 dma_is_single;
};

#define first_entry(x) \
	(struct dl_dma_entry*)((unsigned long)x + sizeof(struct dl_dma_list))
#define next_entry(x) \
	(struct dl_dma_entry*)((unsigned long)x + sizeof(struct dl_dma_entry))
#define get_entry(x, N) \
	(struct dl_dma_entry*)((unsigned long)x + (N * sizeof(struct dl_dma_entry)))

static inline int bmd_to_linux_direction(int direction)
{
	switch (direction)
	{
		case DL_DMA_TO_DEVICE: return PCI_DMA_TODEVICE;
		case DL_DMA_FROM_DEVICE: return PCI_DMA_FROMDEVICE;
		case DL_DMA_BIDIRECTIONAL: return PCI_DMA_BIDIRECTIONAL;
		default:
			break;
	}
	return PCI_DMA_NONE;
}

static unsigned long dl_dma_get_num_pages(void *address, unsigned long size)
{
	unsigned long first = ((unsigned long)address) >> PAGE_SHIFT;
	unsigned long last = ((unsigned long)address + size - 1) >> PAGE_SHIFT;
	return (last - first + 1);
}

static struct dl_dma_list* 
alloc_dl_dma_entry(unsigned long num_pages)
{
	struct dl_dma_list* sl = NULL;

	sl = (struct dl_dma_list*) kzalloc(sizeof(struct dl_dma_list) + (num_pages * sizeof(struct dl_dma_entry)), GFP_KERNEL);
	if (!sl)
		return NULL;

	return sl;
}

static void destroy_dl_dma_entry(struct dl_dma_list* sl)
{
	if (!sl)
		return;
	kfree(sl);
}

struct dl_dma_list* 
dl_dma_map_user_buffer(void* page_array, unsigned long num_pages, int direction, void* pdev)
{
	int i = 0;
	struct dl_dma_list* sl 				= NULL;
	struct dl_dma_entry *e				= NULL;
	struct page** pages = (struct page**)page_array;

	if (!page_array)
		return NULL;

	sl = alloc_dl_dma_entry(num_pages);
	if (!sl)
		return NULL;

	e = first_entry(sl);
	direction = bmd_to_linux_direction(direction);	

	for (i = 0; i < num_pages; i++)
	{
		e->dma_addr = pci_map_page(pdev, pages[i], 0, PAGE_SIZE, direction);
		e = next_entry(e);
	}

	sl->num_pages = num_pages;
	sl->pdev = pdev;	
	
	return sl;
}

struct dl_dma_list* 
dl_dma_map_kernel_buffer(void *address, unsigned long size, int direction, int is_vmalloc, void* pdev)
{
	struct page* page;
	int i = 0, offset = 0;
	struct dl_dma_list* sl 			= NULL;
	struct dl_dma_entry *e			= NULL;
	unsigned long num_pages 		= dl_dma_get_num_pages(address, size);
	unsigned long start_addr		= (unsigned long)address;

	start_addr = start_addr - (start_addr % PAGE_SIZE);

	sl = alloc_dl_dma_entry(is_vmalloc == 1 ? num_pages : 1);
	if (!sl)
		return NULL;

	e = first_entry(sl);
	direction = bmd_to_linux_direction(direction);
	
	if (is_vmalloc)
	{
		for (i = 0; i < num_pages; i++)
		{
			page = vmalloc_to_page((void*)(unsigned long)start_addr + offset);
			offset += PAGE_SIZE;
			e->dma_addr = pci_map_page(pdev, page, 0, PAGE_SIZE, direction);
			e = next_entry(e);
		}
		sl->num_pages = num_pages;
	}
	else
	{
		e->dma_addr = pci_map_single(pdev, address, size, direction);
		sl->dma_is_single = 1;
		sl->size = size;
	}
	sl->pdev = pdev;	
	return sl;
}

dl_dma_addr_t dl_dma_get_physical_segment(struct dl_dma_list* sl, void* address, unsigned long offset, unsigned long* length)
{
	struct dl_dma_entry* e = first_entry(sl);
	unsigned long page_offset;
	unsigned long page_num;
	unsigned long page_oip;
	
	if (sl->dma_is_single)
	{
		if (length)
			*length = sl->size - offset;
		return (dl_dma_addr_t)e->dma_addr + offset;
	}
	
	page_offset = (unsigned long)address % PAGE_SIZE;
	page_num = (page_offset + offset) / PAGE_SIZE;
	page_oip = (page_offset + offset) % PAGE_SIZE;

	if (page_num > sl->num_pages)
		return 0;
	
	e = get_entry(e, page_num);

	if (length)
		*length	= PAGE_SIZE - page_oip;

	return (dl_dma_addr_t)e->dma_addr + page_oip;
}

void dl_dma_unmap_kernel_buffer(struct dl_dma_list* sl, int direction)
{
	unsigned long i;
	struct dl_dma_entry *e = first_entry(sl);
	
	direction = bmd_to_linux_direction(direction);

	if (!sl->dma_is_single)
	{
		for (i = 0; i < sl->num_pages; i++)
		{
			pci_unmap_page(sl->pdev, e->dma_addr, PAGE_SIZE, direction);
			e = next_entry(e);
		}
	}
	else
		pci_unmap_single(sl->pdev, e->dma_addr, sl->size, direction);

	destroy_dl_dma_entry(sl);
}

