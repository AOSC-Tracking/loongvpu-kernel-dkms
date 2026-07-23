#ifndef __HANTRO_HELPER_H__
#define __HANTRO_HELPER_H__

#include <drm/drm_modeset_helper.h>
#include <drm/drm_fourcc.h>
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

static inline void lg_drm_helper_mode_fill_fb_struct(struct drm_device *dev,
                                                struct drm_framebuffer *fb,
                                                const struct drm_format_info *info,
                                                const struct drm_mode_fb_cmd2 *mode_cmd)
{
#if defined(LG_DRM_MODE_CONFIG_FUNCS_FB_CREATE_HAS_INFO)
	return drm_helper_mode_fill_fb_struct(dev, fb, info, mode_cmd);
#else
	return drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);
#endif
}

static inline const struct drm_format_info *lg_drm_get_format_info(struct drm_device *dev,
						const struct drm_mode_fb_cmd2 *mode_cmd)
{
#if defined(LG_DRM_GET_FORMAT_INFO_HAS_PIXEL_FORMAT)
	return drm_get_format_info(dev, mode_cmd->pixel_format, mode_cmd->modifier[0]);
#else
	return drm_get_format_info(dev, mode_cmd);
#endif
}

#endif /* __HANTRO_HELPER_H__ */
