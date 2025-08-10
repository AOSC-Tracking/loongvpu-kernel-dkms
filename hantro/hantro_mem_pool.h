#ifndef _HANTRO_MEM_POOL_H_
#define _HANTRO_MEM_POOL_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/device.h>

typedef struct {
	unsigned long paddr;
	unsigned long vaddr;
	unsigned int size;
	unsigned int used;
} mem_block_t;

#define BK_MPAGES	2048
#define BLOCK_MAX_SIZE	BLOCK_MAX_PAGES * PAGE_SIZE

int memory_pool_alloc(unsigned long *vaddr, unsigned long *busaddr, unsigned int size);
int memory_pool_free(unsigned long addr);
int hantro_mem_pool_init(void);
void hantro_memory_pool_remove(void);

#endif
