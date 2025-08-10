/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited.
 */

#ifndef __GSGPU_VDEC_H__
#define __GSGPU_VDEC_H__
#include <linux/ioctl.h>

#define GSGPU_VDEC_IOC_MAXNR	8
#define GSGPU_VDEC_IOC_MAGIC	'L'
#define GSGPU_VDEC_INIT		_IO(GSGPU_VDEC_IOC_MAGIC, 1)
#define GSGPU_VDEC_GET_TIME	_IO(GSGPU_VDEC_IOC_MAGIC, 2)
#define GSGPU_VDEC_IO_BASE	_IOR(GSGPU_VDEC_IOC_MAGIC, 3, unsigned long *)
#define GSGPU_VDEC_IO_SIZE	_IOR(GSGPU_VDEC_IOC_MAGIC, 4, unsigned int *)
#define GSGPU_VDEC_CLI		_IO(GSGPU_VDEC_IOC_MAGIC,  5)
#define GSGPU_VDEC_STI		_IO(GSGPU_VDEC_IOC_MAGIC,  6)
#define GSGPU_VDEC_ALLOC	_IOWR(GSGPU_VDEC_IOC_MAGIC,  7, unsigned long)
#define GSGPU_VDEC_FREE		_IOW(GSGPU_VDEC_IOC_MAGIC,  8, unsigned long)
#define MAX_OPEN	64
#define ID_INVALID	0xFF
#define GSGPU_VDEC_V	4
#define GSGPU_VDEC_H	240
#define GSGPU_VINT	0x100
#define GSGPU_HINT	0x100

typedef struct {
    char		*buffer;
    unsigned long	addr;
    unsigned int	size;
    void __iomem	*regs;
    int 		irq;
    struct fasync_struct	*aqv;
    struct fasync_struct	*aqh;
} gsgpu_vdec_t;

typedef struct {
    unsigned addr;
    unsigned size;
} mem_param_t;

typedef struct {
    unsigned int addr;
    unsigned int used;
    unsigned int size;
    int id;
} mem_block_t;

#endif /* !__GSGPU_VDEC_H__ */
