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

#endif /* __HANTRO_HELPER_H__ */
