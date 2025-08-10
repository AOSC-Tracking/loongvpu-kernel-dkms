// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro driver on pcie fpga.
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
#include <linux/pci.h>
#include "hantro_pcie.h"

#ifdef VSI_FPGA_PCIE
#define PCI_VENDOR_ID_HANTRO		0x0014 
#define DEC_PCI_DEVICE_ID_HANTRO_PCI	0x7a56		//DEC
#define ENC_PCI_DEVICE_ID_HANTRO_PCI	0x7a66 		//ENC
/* Base address got control register */
#define PCI_H2_BAR			0
/* Base address DDR register */
#define PCI_DDR_BAR			0
#else //customer set own PCIE number
#define PCI_VENDOR_ID_HANTRO	 (0xdeadbeaf)
#define PCI_DEVICE_ID_HANTRO_PCI (0xdeadbeaf)
/* Base address got control register */
#define PCI_H2_BAR				 (0)
/* Base address DDR register */
#define PCI_DDR_BAR				 (0)
#endif

int pcie_init(struct pci_dev *dev_dec, struct hantro_pci_t *pci_par)
{
	struct pci_dev *dev_enc = NULL; /* PCI device structure. */
	unsigned long dec_base_hdwr; /* PCI decoder base register address (Hardware address) */
	unsigned long enc_base_hdwr; /* PCI encoder base register address (Hardware address) */
	u32 dec_base_len; /* Base decoder register address Length */
	u32 enc_base_len; /* Base encoder register address Length */
	int dec_irq = 0;
	int enc_irq = 0;

	if (pci_enable_device(dev_dec) < 0) {
		pr_err("Init: Decoder Device not enabled.\n");
		goto out;
	}
	dec_base_hdwr = pci_resource_start(dev_dec, PCI_H2_BAR);
	if (dec_base_hdwr < 0) {
		pr_info("Init: Decoder Base Address not set.\n");
		goto dec_out_pci_disable_device;
	}
	pr_info("Decoder Base hw val 0x%llx\n", (unsigned long long)dec_base_hdwr);

	dec_base_len = pci_resource_len(dev_dec, PCI_H2_BAR);
	pr_info("Decoder Base hw len 0x%x\n", (unsigned int)dec_base_len);

	dec_irq = pci_irq_vector(dev_dec, 0);
	pci_par->dec_irqnum = dec_irq;

	/* encoder */
	dev_enc = pci_get_device(PCI_VENDOR_ID_HANTRO, ENC_PCI_DEVICE_ID_HANTRO_PCI, dev_enc);
	if (!dev_enc) {
		pr_err("Init: Encoder Hardware not found.\n");
		goto out;
	}

	if (pci_enable_device(dev_enc) < 0) {
		pr_err("Init: Encoder Device not enabled.\n");
		goto out;
	}
	enc_base_hdwr = pci_resource_start(dev_enc, PCI_H2_BAR);
	if (enc_base_hdwr < 0) {
		pr_info("Init: Encoder Base Address not set.\n");
		goto enc_out_pci_disable_device;
	}
	pr_info("Encoder Base hw val 0x%llx\n", (unsigned long long)enc_base_hdwr);

	enc_base_len = pci_resource_len(dev_enc, PCI_H2_BAR);
	pr_info("Encoder Base hw len 0x%x\n", (unsigned int)enc_base_len);

	enc_irq = pci_irq_vector(dev_enc, 0);
	pci_par->enc_irqnum = enc_irq;

	pci_par->dec_pci_base_reg_hw = dec_base_hdwr;
	pci_par->dec_pci_base_reg_len = dec_base_len;
	pci_par->enc_pci_base_reg_hw = enc_base_hdwr;
	pci_par->enc_pci_base_reg_len = enc_base_len;
	pci_par->pci_base_ddr_hw = 0x130000000ULL;
	pci_par->pci_base_ddr_len = 64;
	pci_par->dev = dev_dec;
	pci_par->enc_dev = dev_enc;
	return 0;

dec_out_pci_disable_device:
	pci_disable_device(dev_dec);
enc_out_pci_disable_device:
	pci_disable_device(dev_enc);
out:
	return -1;
}

int pcie_exit(struct pci_dev *dev)
{
//	pci_disable_device(dev);
	return 1;
}
