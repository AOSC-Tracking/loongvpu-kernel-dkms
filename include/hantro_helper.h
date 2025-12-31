#ifndef __HANTRO_HELPER_H__
#define __HANTRO_HELPER_H__

#include "conftest.h"

static inline void lg_vm_flags_set(struct vm_area_struct *vma,
				vm_flags_t flags)
{
#if defined(LG_VM_FLAGS_SET)
	vm_flags_set(vma, flags);
#else
	vma->vm_flags = flags;
#endif
}

#if defined(LG_DRM_IOCTL_FLAGS_UNLOCKED)
#define LG_DRM_UNLOCKED DRM_UNLOCKED
#else
#define LG_DRM_UNLOCKED 0
#endif

static inline int lg_pci_set_dma_mask(struct pci_dev *pci_dev, struct device *dev, u64 mask)
{
#if defined(LG_LINUX_PCI_DMA_COMPAT_H_PRESENT)
	return pci_set_dma_mask(pci_dev, mask);
#else
	return dma_set_mask(dev, mask);
#endif
}

static inline int lg_pci_set_consistent_dma_mask(struct pci_dev *pci_dev, struct device *dev, u64 mask)
{
#if defined(LG_LINUX_PCI_DMA_COMPAT_H_PRESENT)
	return pci_set_consistent_dma_mask(pci_dev, mask);
#else
	return dma_set_coherent_mask(dev, mask);
#endif
}
#endif /* __HANTRO_HELPER_H__ */
