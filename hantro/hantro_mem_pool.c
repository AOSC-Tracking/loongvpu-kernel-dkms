#include "hantro_mem_pool.h"

#define SYSTEM_MEMORY_SIZE 0x40000

static size_t blocks;
static mem_block_t mem_blocks[256];
static spinlock_t alloc_pool_lock;

unsigned int mem_table[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1,
	4, 4, 4, 4, 4, 4, 4, 4, 4,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
	128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
	128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
	128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
	256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
	256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
	256, 256, 256, 256, 256, 256, 256, 256, 256, 256,
	256, 256, 256, 256, 256, 256, 256, 256, 256, 256, /* 4M */
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024,
	1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024
};

extern void si_meminfo(struct sysinfo *val);

int memory_pool_alloc(unsigned long *vaddr, unsigned long *busaddr, unsigned int size)
{
	int i;

	spin_lock(&alloc_pool_lock);
	*busaddr = *vaddr = 0;
	for (i = 0; i < blocks; i++) {
		if (!mem_blocks[i].used && (mem_blocks[i].size >= size)) {
			*busaddr = mem_blocks[i].paddr;
			*vaddr = mem_blocks[i].vaddr;
			mem_blocks[i].used = 1;
			break;
		}
	}

	spin_unlock(&alloc_pool_lock);
	if (*busaddr == 0 || *vaddr == 0) {
		pr_debug("Memory pool alloc failed, size: %d M, try alloc from sys.\n", size/1024/1024);
		return -1;
	}

	return 0;
}

int memory_pool_free(unsigned long addr)
{
	int i;

	for (i = 0; i < blocks; i++) {
		if (mem_blocks[i].vaddr == addr) {
			mem_blocks[i].used = 0;
			return 0;
		}
	}
	return -1;
}

static int memalloc_release(struct inode *inode)
{
	int i;

	for (i = 0; i < blocks; i++)
			mem_blocks[i].used = 0;

	return 0;
}

static int alloc_pages_pool(void)
{
	int i, j;
	struct sysinfo mem_info;
	long total_size = 0;
	bool is_below_4g = 0;

	si_meminfo(&mem_info);

	if (mem_info.freeram < SYSTEM_MEMORY_SIZE)
		is_below_4g = 1;

	for (i = 0; i < blocks; i++) {
		mem_blocks[i].size = PAGE_SIZE * mem_table[i];
		mem_blocks[i].vaddr = (unsigned long)alloc_pages_exact(mem_blocks[i].size, __GFP_ZERO);
		if (!mem_blocks[i].vaddr) {
			for (j = i; j > 0; j--)
				free_pages_exact((void *)mem_blocks[j - 1].vaddr, mem_blocks[j - 1].size);
			return -ENOMEM;
		}
		mem_blocks[i].paddr = __pa(mem_blocks[i].vaddr);
		mem_blocks[i].used = 0;
		total_size += mem_blocks[i].size;
		if (is_below_4g && (i == 127))
			break;
	}
	pr_info("VPU: Reserve totol memory size: %ld M\n", total_size/1024/1024);
	return 0;
}

int hantro_mem_pool_init(void)
{
	int result;

	spin_lock_init(&alloc_pool_lock);
	blocks = (sizeof(mem_table) / sizeof(*mem_table));
	result = alloc_pages_pool();
	if (result) {
		return -ENOMEM;
	}

	return 0;
}

static void free_pages_pool(void)
{
	int i;

	for (i = 0; i < blocks; i++) {
		mem_blocks[i].used = 0;
		free_pages_exact((void *)mem_blocks[i].vaddr, mem_blocks[i].size);
	}
	return;
}

void hantro_memory_pool_remove(void)
{
	free_pages_pool();
}
