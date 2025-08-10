#ifndef __KCL_HELPER_H__
#define __KCL_HELPER_H__

#include "conftest.h"

#if defined (LG_LINUX_DEVICE_CLASS_H_PRESENT)
#include <linux/device/class.h>
#else
#include <linux/device.h>
#endif

static inline struct class * lg_class_create(struct module *owner,
				     const char *name)
{
#if defined(LG_CLASS_CREATE_HAS_OWNER_ARG)
	return class_create(owner, name);
#else
	return class_create(name);
#endif
}

#endif /* __KCL_HELPER_H__ */
