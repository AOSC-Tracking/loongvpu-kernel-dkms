/* SPDX-License-Identifier: GPL-2.0 */
/*
 * header file for pcie hantro fpga.
 *
 * Copyright (c) 2020, VeriSilicon Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 *
 * You may obtain a copy of the GNU General Public License
 * Version 2 at the following locations:
 * https://opensource.org/licenses/gpl-2.0.php
 */

#ifndef __HANTRO_FPGA_PCIE__
#define __HANTRO_FPGA_PCIE__

struct hantro_pci_t {
	struct pci_dev *dev;
	struct pci_dev *enc_dev;
	/* PCI base register address (Hardware address) */
	unsigned long enc_pci_base_reg_hw;
	unsigned long dec_pci_base_reg_hw;
	/* PCI base register address (memalloc) */
	unsigned long pci_base_ddr_hw;
	u32 enc_pci_base_reg_len; /* Base register address Length */
	u32 dec_pci_base_reg_len; /* Base register address Length */
	u32 pci_base_ddr_len; /* Base register address Length */
	u32 enc_irqnum;
	u32 dec_irqnum;
};

/* API */
int pcie_init(struct pci_dev *dev_dec, struct hantro_pci_t *pci_par);
int pcie_exit(struct pci_dev *dev);

#endif /* __HANTRO_FPGA_PCIE__ */
