// SPDX-License-Identifier: GPL-2.0
/*
 *    Hantro driver DMA_BUF fence operation.
 *
 *    Copyright (c) 2017, VeriSilicon Inc.
 *
 *    This program is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU General Public License
 *    as published by the Free Software Foundation; either version 2
 *    of the License, or (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You may obtain a copy of the GNU General Public License
 *    Version 2 or later at the following locations:
 *    http://www.opensource.org/licenses/gpl-license.html
 *    http://www.gnu.org/copyleft/gpl.html
 */

#include "hantro_priv.h"
#include "hantrodec.h"
#include "hx280enc.h"
#include "hantrocache.h"
#include "hantrodec400.h"
#include "hantro_vcmd.h"
#include "hantrommu.h"
#include "hantro.h"
#include "hantro_helper.h"
#include "hantro_mem_pool.h"

#ifdef __amd64__
#include <asm/set_memory.h>
#endif

#ifdef VSI_FPGA_MEM
#include "hantro_fpga_mem.h"
#endif

#ifdef PCIE_EN
#include "hantro_pcie.h"
#endif

#define DRIVER_DESC "hantro DRM"
#define DRIVER_DATE "20251203"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 1

static DEFINE_MUTEX(m_global_lock);

#if defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
static const struct drm_gem_object_funcs hantro_drm_gem_cma_funcs;
#endif
static int AllocateMem(struct drm_gem_hantro_object *cma_obj, struct drm_mode_create_dumb *args);
static void FreeMem(struct drm_gem_hantro_object *cma_obj);
static int hantro_drm_gem_mmap_obj(struct drm_gem_object *obj, unsigned long obj_size, struct vm_area_struct *vma);

static void hantro_drm_fb_destroy(struct drm_framebuffer *fb)
{
	struct hantro_drm_fb *vsi_fb = (struct hantro_drm_fb *)fb;
	int i;

	for (i = 0; i < 4; i++)
		hantro_unref_drmobj(vsi_fb->obj[i]);

	drm_framebuffer_cleanup(fb);
	kfree(vsi_fb);
}

static int hantro_drm_fb_create_handle(struct drm_framebuffer *fb,
				       struct drm_file *file_priv,
				       unsigned int *handle)
{
	struct hantro_drm_fb *vsi_fb = (struct hantro_drm_fb *)fb;

	return drm_gem_handle_create(file_priv, vsi_fb->obj[0], handle);
}

static int hantro_drm_fb_dirty(struct drm_framebuffer *fb,
			       struct drm_file *file, unsigned int flags,
			       unsigned int color, struct drm_clip_rect *clips,
			       unsigned int num_clips)
{
	/*nothing to do now*/
	return 0;
}

static const struct drm_framebuffer_funcs hantro_drm_fb_funcs = {
	.destroy = hantro_drm_fb_destroy,
	.create_handle = hantro_drm_fb_create_handle,
	.dirty = hantro_drm_fb_dirty,
};

static int hantro_gem_dumb_create_internal(struct drm_file *file_priv,
					   struct drm_device *dev,
					   struct drm_mode_create_dumb *args)
{
	int ret = 0;
	int in_size, out_size;
	struct drm_gem_hantro_object *cma_obj;
	struct drm_gem_object *obj;

	int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	unsigned int sliceidx = args->handle;
	unsigned int config;

	struct slice_info *pslice = getslicenode(sliceidx);

	if (!pslice)
		return -EINVAL;

	config = pslice->config;

	args->handle = 0;

	cma_obj = kzalloc(sizeof(*cma_obj), GFP_KERNEL);
	if (!cma_obj) {
		ret = -ENOMEM;
		goto out;
	}
	obj = &cma_obj->base;
	cma_obj->dmapriv.self = cma_obj;
	in_size = sizeof(*args);
	out_size = in_size;
	args->pitch = ALIGN(min_pitch, 64);
	args->size = (__u64)args->pitch * (__u64)args->height;
	args->size = PAGE_ALIGN(args->size);

	cma_obj->num_pages = args->size >> PAGE_SHIFT;
	cma_obj->flag = 0;
	cma_obj->pageaddr = NULL;
	cma_obj->vaddr = NULL;
	cma_obj->sliceidx = sliceidx;

	ret = AllocateMem(cma_obj, args);
	if (ret != 0) {
		kfree(cma_obj);
		ret = -ENOMEM;
		goto out;
	}

#if defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
	if (!obj->funcs)
		obj->funcs = &hantro_drm_gem_cma_funcs;
#endif
	drm_gem_object_init(dev, obj, args->size);
	ret = drm_gem_handle_create(file_priv, obj, &args->handle);
	hantro_unref_drmobj(obj);

	if (ret) {
		FreeMem(cma_obj);
		kfree(cma_obj);
		goto out;
	}
	init_hantro_resv(&cma_obj->kresv, cma_obj);
	cma_obj->handle = args->handle;

out:
	return ret;
}

static int hantro_gem_dumb_create(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	return hantro_gem_dumb_create_internal(
		file_priv, dev, (struct drm_mode_create_dumb *)data);
}

static int hantro_gem_dumb_map_offset(struct drm_file *file_priv,
				      struct drm_device *dev, uint32_t handle,
				      uint64_t *offset)
{
	struct drm_gem_object *obj;
	int ret;

	obj = hantro_gem_object_lookup(dev, file_priv, handle);
	if (!obj)
		return -EINVAL;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret == 0) {
		*offset = (drm_vma_node_offset_addr(&obj->vma_node) + VSI_MMAP_ADDRES_CEIL);
		hantro_mmaplog("create normal map offset %llx", *offset);
	}

	hantro_unref_drmobj(obj);
	return ret;
}

static int hantro_gem_dumb_destroy(struct drm_file *file,
				   struct drm_device *dev, u32 handle)
{
	return drm_gem_handle_delete(file, handle);
}

static int hantro_destroy_dumb(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_mode_destroy_dumb *args = data;
	struct drm_gem_object *obj;
	struct drm_gem_hantro_object *cma_obj;

	obj = hantro_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj) {
		return -EINVAL;
	}
	hantro_unref_drmobj(obj);

	cma_obj = to_drm_gem_hantro_obj(obj);

	drm_gem_handle_delete(file_priv, args->handle);
	return 0;
}

static struct sg_table *
hantro_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct drm_gem_hantro_object *cma_obj = to_drm_gem_hantro_obj(obj);
	struct slice_info *pslice = getslicenode(cma_obj->sliceidx);
	struct sg_table *sgt;
	int ret;

	if (!pslice)
		return NULL;
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;

	ret = dma_get_sgtable(pslice->dev, sgt, cma_obj->vaddr, cma_obj->paddr,
			      obj->size);
	if (ret < 0) {
		kfree(sgt);
		sgt = NULL;
	}
	return sgt;
}

static struct drm_gem_object *
hantro_gem_prime_import_sg_table(struct drm_device *dev,
				 struct dma_buf_attachment *attach,
				 struct sg_table *sgt)
{
	struct drm_gem_hantro_object *cma_obj;
	struct drm_gem_object *obj;
#if !defined(LG_DMA_BUF_OPS_VMAP_ARG_DMA_BUF)
	struct iosys_map map;
#endif

	cma_obj = kzalloc(sizeof(*cma_obj), GFP_KERNEL);
	if (!cma_obj)
		return ERR_PTR(-ENOMEM);

	obj = &cma_obj->base;

	if (!(hantro_dev.config & CONFIG_HANTROMMU) && sgt->nents > 1) {
		/* check if the entries in the sg_table are contiguous */
		dma_addr_t next_addr = sg_dma_address(sgt->sgl);
		struct scatterlist *s;
		unsigned int i;

		for_each_sg(sgt->sgl, s, sgt->nents, i) {
			/*
			 * sg_dma_address(s) is only valid for entries
			 * that have sg_dma_len(s) != 0
			 */
			if (!sg_dma_len(s))
				continue;

			if (sg_dma_address(s) != next_addr) {
				kfree(cma_obj);
				return ERR_PTR(-EINVAL);
			}

			next_addr = sg_dma_address(s) + sg_dma_len(s);
		}
	}

#if defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
	if (!obj->funcs)
		obj->funcs = &hantro_drm_gem_cma_funcs;
#endif

	if (drm_gem_object_init(dev, obj, attach->dmabuf->size) != 0) {
		kfree(cma_obj);
		return ERR_PTR(-ENOMEM);
	}
	cma_obj->paddr = sg_dma_address(sgt->sgl);
#if defined(LG_DMA_BUF_OPS_VMAP_ARG_DMA_BUF)
	cma_obj->vaddr = dma_buf_vmap(attach->dmabuf);
#else
	dma_buf_vmap(attach->dmabuf, &map);
	cma_obj->vaddr = map.vaddr;
#endif
	cma_obj->sgt = sgt;
	cma_obj->num_pages = attach->dmabuf->size >> PAGE_SHIFT;
	cma_obj->dmapriv.meta_data =
		*((struct viv_vidmem_metadata *)attach->dmabuf->priv);
	cma_obj->dmapriv.self = cma_obj;
	cma_obj->dmapriv.meta_data.magic = HANTRO_IMAGE_VIV_META_DATA_MAGIC;
	cma_obj->flag |= HANTRO_GEM_FLAG_IMPORT;
	if (hantro_dev.config & CONFIG_HANTROMMU)
		cma_obj->flag |= HANTRO_GEM_FLAG_USEVMALLOC; //make it use MMU

	return obj;
}

#if defined(LG_DMA_BUF_OPS_VMAP_ARG_DMA_BUF)
static void *hantro_gem_prime_vmap(struct drm_gem_object *obj)
{
	struct drm_gem_hantro_object *cma_obj = to_drm_gem_hantro_obj(obj);

	return cma_obj->vaddr;
}

static void hantro_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
}

#else
static int hantro_gem_prime_vmap(struct drm_gem_object *obj,
				 struct iosys_map *map)
{
	struct drm_gem_hantro_object *cma_obj = to_drm_gem_hantro_obj(obj);

	iosys_map_set_vaddr(map, cma_obj->vaddr);

	return 0;
}

static void hantro_gem_prime_vunmap(struct drm_gem_object *obj,
				   struct iosys_map *map)
{
}
#endif

/* omitted in kernel version > 5.4.0
 *static struct reservation_object *hantro_gem_prime_res_obj(
 *	struct drm_gem_object *obj)
 *{
 *	struct drm_gem_hantro_object *hobj = to_drm_gem_hantro_obj(obj);
 *
 *	return &hobj->kresv;
 *}
 */

int hantro_gem_prime_mmap(struct drm_gem_object *obj,
				 struct vm_area_struct *vma)
{
	struct drm_gem_hantro_object *cma_obj;
	unsigned long page_num = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	int ret = 0;

	cma_obj = to_drm_gem_hantro_obj(obj);
	if (page_num > cma_obj->num_pages)
		return -EINVAL;

	if ((unsigned long)cma_obj->vaddr == 0)
		return -EINVAL;

	ret = hantro_drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret < 0)
		return ret;
	lg_vm_flags_set(vma, vma->vm_flags & ~VM_PFNMAP);
	vma->vm_pgoff = 0;
	if (mutex_lock_interruptible(&hantro_dev.struct_mutex))
		return -EBUSY;
	if (dma_mmap_coherent(obj->dev->dev, vma, cma_obj->vaddr,
			      cma_obj->paddr, vma->vm_end - vma->vm_start)) {
		drm_gem_vm_close(vma);
		mutex_unlock(&hantro_dev.struct_mutex);
		return -EAGAIN;
	}
	mutex_unlock(&hantro_dev.struct_mutex);
	vma->vm_private_data = cma_obj;
	return ret;
}

static void hantro_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct drm_gem_hantro_object *cma_obj;
	/* dma buf imported from others,
	 * release data structures allocated by ourselves
	 */
	cma_obj = to_drm_gem_hantro_obj(gem_obj);

	if (gem_obj->import_attach) {
		if (cma_obj->vaddr)
			dma_buf_vunmap(gem_obj->import_attach->dmabuf,
				       cma_obj->vaddr);
		drm_prime_gem_destroy(gem_obj, cma_obj->sgt);
	} else if (cma_obj->vaddr) {
		FreeMem(cma_obj);
	}
#if !defined(LG_LINUX_DMA_RESV_H_PRESENT)
	reservation_object_fini(&cma_obj->kresv);
#else
	dma_resv_fini(&cma_obj->kresv);
#endif


	drm_gem_object_release(gem_obj);
	kfree(cma_obj);
}

static int hantro_gem_close(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_gem_close *args = data;
	int ret = 0;
	struct drm_gem_object *obj =
		hantro_gem_object_lookup(dev, file_priv, args->handle);

	if (!obj)
		return -EINVAL;

	ret = drm_gem_handle_delete(file_priv, args->handle);
	hantro_unref_drmobj(obj);
	return ret;
}

static int hantro_gem_open(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	int ret;
	u32 handle;
	struct drm_gem_open *openarg;
	struct drm_gem_object *obj = NULL;

	openarg = (struct drm_gem_open *)data;

	obj = idr_find(&dev->object_name_idr, (int)openarg->name);
	if (obj)
		hantro_ref_drmobj(obj);
	else
		return -ENOENT;

	ret = drm_gem_handle_create(file_priv, obj, &handle);
	hantro_unref_drmobj(obj);
	if (ret)
		return ret;

	openarg->handle = handle;
	openarg->size = obj->size;

	return ret;
}

static int hantro_map_vaddr(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct hantro_addrmap *pamap = data;
	struct drm_gem_object *obj;
	struct drm_gem_hantro_object *cma_obj;

	obj = hantro_gem_object_lookup(dev, file_priv, pamap->handle);
	if (!obj)
		return -EINVAL;

	cma_obj = to_drm_gem_hantro_obj(obj);
	pamap->vm_addr = (unsigned long long)cma_obj->vaddr;
	pamap->phy_addr = cma_obj->paddr;
	pamap->mem_base = cma_obj->mem_base;

	hantro_unref_drmobj(obj);
	return 0;
}

static int hantro_get_hwcfg(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	return hantro_dev.config;
}

static int hantro_get_slicenum(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	int vcmd_en;
	hantro_ioctl_id ioctl_id_par;

	ioctl_id_par.ID_PAR.node_idx = 0;
	vcmd_en = hantro_get_vcmdsup(NULL, &ioctl_id_par.data, NULL);

#ifdef HAS_VCMD
		return get_vcmd_slice_num();
#endif
		return get_slicenumber();
}

/*reference linux 4.11. ubuntu 16.04 have issues in its drm_gem_flink_ioctl().
 * MODIFICATION:
 * drm_gem_object_lookup(file_priv, args->handle);
 * => drm_gem_object_lookup(dev, file_priv, args->handle);
 */
static int hantro_gem_flink(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_gem_flink *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_GEM))
		return -ENODEV;

	obj = hantro_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	mutex_lock(&dev->object_name_lock);
	/* prevent races with concurrent gem_close. */
	if (obj->handle_count == 0) {
		ret = -ENOENT;
		goto err;
	}

	if (!obj->name) {
		ret = idr_alloc(&dev->object_name_idr, obj, 1, 0, GFP_KERNEL);
		if (ret < 0)
			goto err;

		obj->name = ret;
	}

	args->name = (uint64_t)obj->name;
	ret = 0;

err:
	mutex_unlock(&dev->object_name_lock);
	hantro_unref_drmobj(obj);
	return ret;
}

static int hantro_map_dumb(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	int ret;
	struct drm_mode_map_dumb *temparg = (struct drm_mode_map_dumb *)data;

	ret = hantro_gem_dumb_map_offset(file_priv, dev, temparg->handle,
					 &temparg->offset);

	return ret;
}

static int hantro_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct idr *ptr;

	ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;
	idr_init(ptr);
	file->driver_priv = ptr;
	return 0;
}

static struct drm_gem_object *
hantro_drm_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
#if !defined(LG_DRM_GEM_OBJECT_HAS_RESV)
    struct drm_gem_object *obj;

    obj = dma_buf->priv;
    if (obj->dev == dev)
    {
        printk("import from orig driver\n");
    }
    return drm_gem_prime_import(dev, dma_buf);
#else
	struct device *attach_dev = dev->dev;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct drm_gem_object *obj;
	int ret;

	if (dma_buf->ops == &hantro_dmabuf_ops) {
		obj = hantro_get_gem_from_dmabuf(dma_buf);
		if (obj && obj->dev == dev) {
			drm_gem_object_get(obj);
			return obj;
		}
	}

	if (!dev->driver->gem_prime_import_sg_table)
		return ERR_PTR(-EINVAL);

	attach = dma_buf_attach(dma_buf, attach_dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto fail_detach;
	}

	obj = dev->driver->gem_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		goto fail_unmap;
	}

	obj->import_attach = attach;
	obj->resv = dma_buf->resv;

	return obj;

fail_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ERR_PTR(ret);
#endif
}

static struct dma_buf *hantro_prime_export(struct drm_gem_object *obj,
					   int flags)
{
	struct dma_buf *dma_buf;
	struct drm_gem_hantro_object *cma_obj;
	struct drm_device *dev = obj->dev;
	struct dma_buf_export_info exp_info = {
		.exp_name = KBUILD_MODNAME,
		.owner = dev->driver->fops->owner,
		.ops = &hantro_dmabuf_ops,
		.flags = flags,
	};

	cma_obj = to_drm_gem_hantro_obj(obj);
	exp_info.resv = &cma_obj->kresv;
	exp_info.size = cma_obj->num_pages << PAGE_SHIFT;
	exp_info.priv = &cma_obj->dmapriv.meta_data;

	dma_buf = dma_buf_export(&exp_info);
	if (IS_ERR(dma_buf))
		return dma_buf;

	drm_dev_get(dev);
	drm_gem_object_get(&cma_obj->base);
	//dma_buf->file->f_mapping = obj->dev->anon_inode->i_mapping;
	//just a remind

	return dma_buf;
}

static int hantro_handle_to_fd(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	int ret;
	struct drm_prime_handle *primeargs = (struct drm_prime_handle *)data;
	struct drm_gem_object *obj;
	struct drm_gem_hantro_object *cma_obj;

	obj = hantro_gem_object_lookup(dev, file_priv, primeargs->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_prime_handle_to_fd(dev, file_priv, primeargs->handle,
					 primeargs->flags, &primeargs->fd);

	if (ret == 0) {
		cma_obj = to_drm_gem_hantro_obj(obj);
		cma_obj->flag |= HANTRO_GEM_FLAG_EXPORT;
	}
	hantro_unref_drmobj(obj);
	return ret;
}

static int hantro_fd_to_handle(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_prime_handle *primeargs = (struct drm_prime_handle *)data;
	s32 ret = 0;
	struct dma_buf *dma_buf;
	struct drm_gem_object *obj;
	struct drm_gem_hantro_object *cma_obj;

	primeargs->flags = 0;
	ret = drm_gem_prime_fd_to_handle(dev, file_priv, primeargs->fd,
					 &primeargs->handle);

	/*Just for debugging to get object*/
	if (ret == 0) {
		dma_buf = dma_buf_get(primeargs->fd);
		if (IS_ERR(dma_buf))
			return PTR_ERR(dma_buf);
		obj = dma_buf->priv;
		cma_obj = to_drm_gem_hantro_obj(obj);
		dma_buf_put(dma_buf);
//		pr_debug(
//			"client[%p]import  gem name[%u] gem size[%zu] handle_cnt[%d] refcnt[%d] flag[0x%x]\n",
//			file_priv, obj->name, obj->size, obj->handle_count,
//			kref_read(&obj->refcount), cma_obj->flag);
	}

	return ret;
}

static int hantro_fb_create2(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct drm_mode_fb_cmd2 *mode_cmd = (struct drm_mode_fb_cmd2 *)data;
	struct hantro_drm_fb *vsifb;
	struct drm_gem_object *objs[4];
	struct drm_gem_object *obj;
#if defined(LG_DRM_GET_FORMAT_INFO_USE_PIXEL_FORMAT)
	const struct drm_format_info *info = drm_get_format_info(dev, mode_cmd->pixel_format, mode_cmd->modifier[0]);
#else
	const struct drm_format_info *info = drm_get_format_info(dev, mode_cmd);
#endif
	unsigned int hsub;
	unsigned int vsub;
	int num_planes;
	int ret;
	int i;

	hsub = info->hsub;
	vsub = info->vsub;
	num_planes = min_t(int, info->num_planes, 4);
	for (i = 0; i < num_planes; i++) {
		unsigned int width = mode_cmd->width / (i ? hsub : 1);
		unsigned int height = mode_cmd->height / (i ? vsub : 1);
		unsigned int min_size;

		obj = hantro_gem_object_lookup(dev, file_priv,
					       mode_cmd->handles[i]);
		if (!obj) {
			ret = -ENXIO;
			goto err_gem_object_unreference;
		}
		hantro_unref_drmobj(obj);
		min_size = (height - 1) * mode_cmd->pitches[i] +
			   mode_cmd->offsets[i] +
			   width * info->cpp[i];
		if (obj->size < min_size) {
			//hantro_unref_drmobj(obj);
			ret = -EINVAL;
			goto err_gem_object_unreference;
		}
		objs[i] = obj;
	}
	vsifb = kzalloc(sizeof(*vsifb), GFP_KERNEL);
	if (!vsifb)
		return -ENOMEM;
#if defined(LG_DRM_HELPER_MODE_FILL_FB_STRUCT_PASSES_INFO)
	drm_helper_mode_fill_fb_struct(dev, &vsifb->fb, NULL, mode_cmd);
#else
	drm_helper_mode_fill_fb_struct(dev, &vsifb->fb, mode_cmd);
#endif
	for (i = 0; i < num_planes; i++)
		vsifb->obj[i] = objs[i];
	ret = drm_framebuffer_init(dev, &vsifb->fb, &hantro_drm_fb_funcs);
	if (ret)
		kfree(vsifb);
	return ret;

err_gem_object_unreference:
	for (i--; i >= 0; i--)
		; //hantro_unref_drmobj(objs[i]);

	return ret;
}

static int hantro_fb_create(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_mode_fb_cmd *cmd_or = data;
	struct drm_mode_fb_cmd2 r = {};
	int ret;

	/* convert to new format and call new ioctl */
	r.fb_id = cmd_or->fb_id;
	r.width = cmd_or->width;
	r.height = cmd_or->height;
	r.pitches[0] = cmd_or->pitch;
	r.pixel_format = drm_mode_legacy_fb_format(cmd_or->bpp, cmd_or->depth);
	r.handles[0] = cmd_or->handle;

	ret = hantro_fb_create2(dev, &r, file_priv);
	if (ret)
		return ret;

	cmd_or->fb_id = r.fb_id;

	return 0;
}

static int hantro_get_version(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_version *pversion;
	char *sname = DRIVER_NAME;
	char *sdesc = DRIVER_DESC;
	char *sdate = DRIVER_DATE;

	pversion = (struct drm_version *)data;
	pversion->version_major = dev->driver->major;
	pversion->version_minor = dev->driver->minor;
	pversion->version_patchlevel = 0;
	pversion->name_len = strlen(DRIVER_NAME);
	pversion->desc_len = strlen(DRIVER_DESC);
	pversion->date_len = strlen(DRIVER_DATE);
	if (pversion->name)
		if (copy_to_user(pversion->name, sname, pversion->name_len))
			return -EFAULT;
	if (pversion->date)
		if (copy_to_user(pversion->date, sdate, pversion->date_len))
			return -EFAULT;
	if (pversion->desc)
		if (copy_to_user(pversion->desc, sdesc, pversion->desc_len))
			return -EFAULT;
	return 0;
}

static int hantro_get_cap(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_get_cap *req = (struct drm_get_cap *)data;

	req->value = 0;
	/*some values should be reset*/
	switch (req->capability) {
	case DRM_CAP_PRIME:
		req->value |= dev->driver->prime_fd_to_handle ?
				      DRM_PRIME_CAP_IMPORT :
				      0;
		req->value |= dev->driver->prime_handle_to_fd ?
				      DRM_PRIME_CAP_EXPORT :
				      0;
		return 0;
	case DRM_CAP_DUMB_BUFFER:
		req->value = 1;
		break;
	case DRM_CAP_VBLANK_HIGH_CRTC:
		req->value = 1;
		break;
	case DRM_CAP_DUMB_PREFERRED_DEPTH:
		req->value = dev->mode_config.preferred_depth;
		break;
	case DRM_CAP_DUMB_PREFER_SHADOW:
		req->value = dev->mode_config.prefer_shadow;
		break;
	case DRM_CAP_ASYNC_PAGE_FLIP:
		req->value = dev->mode_config.async_page_flip;
		break;
	case DRM_CAP_CURSOR_WIDTH:
		if (dev->mode_config.cursor_width)
			req->value = dev->mode_config.cursor_width;
		else
			req->value = 64;
		break;
	case DRM_CAP_CURSOR_HEIGHT:
		if (dev->mode_config.cursor_height)
			req->value = dev->mode_config.cursor_height;
		else
			req->value = 64;
		break;
	case DRM_CAP_ADDFB2_MODIFIERS:
#if defined(LG_DRM_MODE_CONFIG_HAS_FB_MOD)
		req->value = dev->mode_config.allow_fb_modifiers;
#else
		// for kernel 6.1 compile
		req->value = dev->mode_config.fb_modifiers_not_supported;
#endif
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*just a test API for any purpose*/
static int hantro_test(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	unsigned int *input = data;
	int handle = *input;
	struct drm_gem_object *obj;
	hantro_fence_t *pfence;
	int ret = 10 * HZ; /*timeout*/

	obj = hantro_gem_object_lookup(dev, file_priv, handle);
	if (!obj)
		return -EINVAL;

#if !defined(LG_LINUX_DMA_RESV_H_PRESENT)
    pfence = reservation_object_get_excl(obj->dma_buf->resv);
#elif defined(LG_DMA_RESV_GET_EXCL_PRESENT)
	pfence = dma_resv_get_excl(obj->dma_buf->resv);
#else
	// for kernel 6.1 compile
	hantro_fence_t **fence;

	dma_resv_get_fences(obj->dma_buf->resv, 0, NULL, &fence);
	pfence = *fence;
#endif
	while (ret > 0)
		ret = schedule_timeout(ret);

	hantro_fence_signal(pfence);
	hantro_unref_drmobj(obj);
	return 0;
}

static int hantro_getprimeaddr(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	unsigned long *input = data;
	int fd = *input;
	struct drm_gem_hantro_object *cma_obj;
	struct dma_buf *dma_buf;

	dma_buf = dma_buf_get(fd);
	if (IS_ERR(dma_buf))
		return PTR_ERR(dma_buf);

	cma_obj = (struct drm_gem_hantro_object *)dma_buf->priv;
	*input = cma_obj->paddr;
	dma_buf_put(dma_buf);
	return 0;
}

static int hantro_ptr_to_phys(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	unsigned long *arg = data;
	struct vm_area_struct *vma;
	struct drm_gem_hantro_object *cma_obj;
	unsigned long vaddr = *arg;

	vma = find_vma(current->mm, vaddr);
	if (!vma)
		return -EFAULT;

	cma_obj = (struct drm_gem_hantro_object *)vma->vm_private_data;
	if (!cma_obj)
		return -EFAULT;

	if (cma_obj->base.dev != dev)
		return -EFAULT;

	if (vaddr < vma->vm_start ||
	    vaddr >= vma->vm_start + (cma_obj->num_pages << PAGE_SHIFT))
		return -EFAULT;

	*arg = (phys_addr_t)(vaddr - vma->vm_start) + cma_obj->paddr;
	return 0;
}

static int hantro_query_metadata(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct hantro_metadata_params *metadata_p =
		(struct hantro_metadata_params *)data;
	struct drm_gem_object *obj = NULL;
	struct drm_gem_hantro_object *cma_obj = NULL;

	obj = hantro_gem_object_lookup(dev, file_priv, metadata_p->handle);
	if (!obj)
		return -ENOENT;
	cma_obj = to_drm_gem_hantro_obj(obj);

	memcpy(&metadata_p->meta_data, &cma_obj->dmapriv.meta_data,
	       sizeof(struct viv_vidmem_metadata));

	hantro_unref_drmobj(obj);

	return 0;
}

static int hantro_update_metadata(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	struct hantro_metadata_params *metadata_p =
		(struct hantro_metadata_params *)data;
	struct drm_gem_object *obj = NULL;
	struct drm_gem_hantro_object *cma_obj = NULL;

	obj = hantro_gem_object_lookup(dev, file_priv, metadata_p->handle);
	if (!obj)
		return -ENOENT;
	cma_obj = to_drm_gem_hantro_obj(obj);

	memcpy(&cma_obj->dmapriv.meta_data, &metadata_p->meta_data,
	       sizeof(struct viv_vidmem_metadata));

	hantro_unref_drmobj(obj);

	return 0;
}

static int hantro_getmagic(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_auth *auth = data;
	int ret = 0;

	mutex_lock(&dev->master_mutex);
	if (!file_priv->magic) {
		ret = idr_alloc(&file_priv->master->magic_map, file_priv, 1, 0,
				GFP_KERNEL);
		if (ret >= 0)
			file_priv->magic = ret;
	}
	auth->magic = file_priv->magic;
	DBG("kmagic %d\n", auth->magic);
	mutex_unlock(&dev->master_mutex);

	return ret < 0 ? ret : 0;
}

static int hantro_authmagic(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_auth *auth = data;
	struct drm_file *file;

	mutex_lock(&dev->master_mutex);
	file = idr_find(&file_priv->master->magic_map, auth->magic);
	DBG("get kmagic %d\n", auth->magic);
	if (file) {
		file->authenticated = 1;
		idr_replace(&file_priv->master->magic_map, NULL, auth->magic);
	}
	mutex_unlock(&dev->master_mutex);

	return file ? 0 : -EINVAL;
}

#define DRM_IOCTL_DEF(ioctl, _func, _flags)                                    \
	[DRM_IOCTL_NR(ioctl)] = {                                              \
		.cmd = ioctl, .func = _func, .flags = _flags, .name = #ioctl   \
	}

/*after kernel 4.16 this definition is removed*/
#ifndef DRM_CONTROL_ALLOW
#define DRM_CONTROL_ALLOW 0
#endif
/* Ioctl table */
static const struct drm_ioctl_desc hantro_ioctls[] = {
	DRM_IOCTL_DEF(DRM_IOCTL_VERSION, hantro_get_version,
		      LG_DRM_UNLOCKED | DRM_RENDER_ALLOW | DRM_CONTROL_ALLOW),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_UNIQUE, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_MAGIC, hantro_getmagic, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_IRQ_BUSID, drm_invalid_op,
		      DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_MAP, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_CLIENT, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_STATS, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_CAP, hantro_get_cap,
		      LG_DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF(DRM_IOCTL_SET_CLIENT_CAP, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_SET_VERSION, drm_invalid_op,
		      LG_DRM_UNLOCKED | DRM_MASTER),

	DRM_IOCTL_DEF(DRM_IOCTL_SET_UNIQUE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_BLOCK, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_UNBLOCK, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AUTH_MAGIC, hantro_authmagic,
		      DRM_AUTH | LG_DRM_UNLOCKED | DRM_MASTER),

	DRM_IOCTL_DEF(DRM_IOCTL_ADD_MAP, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_RM_MAP, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_SET_SAREA_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_SAREA_CTX, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_SET_MASTER, drm_invalid_op,
		      LG_DRM_UNLOCKED | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_DROP_MASTER, drm_invalid_op,
		      LG_DRM_UNLOCKED | DRM_ROOT_ONLY),

	DRM_IOCTL_DEF(DRM_IOCTL_ADD_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_RM_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_MOD_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_GET_CTX, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_SWITCH_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_NEW_CTX, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_RES_CTX, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_ADD_DRAW, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_RM_DRAW, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),

	DRM_IOCTL_DEF(DRM_IOCTL_LOCK, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_UNLOCK, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_FINISH, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_ADD_BUFS, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_MARK_BUFS, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_INFO_BUFS, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_MAP_BUFS, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_FREE_BUFS, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_DMA, drm_invalid_op, DRM_AUTH),

	DRM_IOCTL_DEF(DRM_IOCTL_CONTROL, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),

#if IS_ENABLED(CONFIG_AGP)
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_ACQUIRE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_RELEASE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_ENABLE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_INFO, drm_invalid_op, DRM_AUTH),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_ALLOC, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_FREE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_BIND, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_AGP_UNBIND, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
#endif

	DRM_IOCTL_DEF(DRM_IOCTL_SG_ALLOC, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF(DRM_IOCTL_SG_FREE, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),

	DRM_IOCTL_DEF(DRM_IOCTL_WAIT_VBLANK, drm_invalid_op, LG_DRM_UNLOCKED),

	DRM_IOCTL_DEF(DRM_IOCTL_MODESET_CTL, drm_invalid_op, 0),

	DRM_IOCTL_DEF(DRM_IOCTL_UPDATE_DRAW, drm_invalid_op,
		      DRM_AUTH | DRM_MASTER | DRM_ROOT_ONLY),

	DRM_IOCTL_DEF(DRM_IOCTL_GEM_CLOSE, hantro_gem_close,
		      LG_DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF(DRM_IOCTL_GEM_FLINK, hantro_gem_flink,
		      DRM_AUTH | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_GEM_OPEN, hantro_gem_open,
		      DRM_AUTH | LG_DRM_UNLOCKED),

	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETRESOURCES, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),

	DRM_IOCTL_DEF(DRM_IOCTL_PRIME_HANDLE_TO_FD, hantro_handle_to_fd,
		      DRM_AUTH | LG_DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF(DRM_IOCTL_PRIME_FD_TO_HANDLE, hantro_fd_to_handle,
		      DRM_AUTH | LG_DRM_UNLOCKED | DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPLANERESOURCES, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETCRTC, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETCRTC, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPLANE, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETPLANE, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_CURSOR, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETGAMMA, drm_invalid_op, LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETGAMMA, drm_invalid_op,
		      DRM_MASTER | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETENCODER, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETCONNECTOR, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_ATTACHMODE, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_DETACHMODE, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPROPERTY, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_SETPROPERTY, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETPROPBLOB, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_GETFB, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_ADDFB, hantro_fb_create,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_ADDFB2, hantro_fb_create2,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_RMFB, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_PAGE_FLIP, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_DIRTYFB, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATE_DUMB, hantro_gem_dumb_create,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_MAP_DUMB, hantro_map_dumb,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_DESTROY_DUMB, hantro_destroy_dumb,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_OBJ_SETPROPERTY, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_CURSOR2, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_ATOMIC, drm_invalid_op,
		      DRM_MASTER | DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATEPROPBLOB, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_MODE_DESTROYPROPBLOB, drm_invalid_op,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),

	/*hantro specific ioctls*/
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_TESTCMD, hantro_test,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GETPADDR, hantro_map_vaddr,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_HWCFG, hantro_get_hwcfg,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_TESTREADY, hantro_testbufvalid,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_SETDOMAIN, hantro_setdomain,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_ACQUIREBUF, hantro_acquirebuf,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_RELEASEBUF, hantro_releasebuf,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GETPRIMEADDR, hantro_getprimeaddr,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_PTR_PHYADDR, hantro_ptr_to_phys,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_QUERY_METADATA, hantro_query_metadata,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_UPDATE_METADATA, hantro_update_metadata,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),

	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GET_SLICENUM, hantro_get_slicenum,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GET_VCMDSUP, hantro_get_vcmdsup,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GET_IRQINFO, hantro_get_irqinfo,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
	DRM_IOCTL_DEF(DRM_IOCTL_HANTRO_GET_PMSUP, hantro_get_pmsup,
		      DRM_CONTROL_ALLOW | LG_DRM_UNLOCKED),
};

#if DRM_CONTROL_ALLOW == 0
#undef DRM_CONTROL_ALLOW
#endif

#define HANTRO_IOCTL_COUNT ARRAY_SIZE(hantro_ioctls)
static long hantro_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = hantro_dev.drm_dev;
	const struct drm_ioctl_desc *ioctl = NULL;
	drm_ioctl_t *func;
	unsigned int nr = _IOC_NR(cmd);
	int retcode = 0;
	char stack_kdata[256];
	char *kdata = stack_kdata;
	unsigned int in_size, out_size;

	if (drm_dev_is_unplugged(dev))
		return -ENODEV;

	in_size = _IOC_SIZE(cmd);
	out_size = in_size;
	pr_debug("ioctl cmd %d:%d\n", _IOC_TYPE(cmd), nr);

	if (in_size > 0) {
		if (_IOC_DIR(cmd) & _IOC_READ)
			retcode = !hantro_access_ok(VERIFY_WRITE, (void *)arg,
						    in_size);
		else if (_IOC_DIR(cmd) & _IOC_WRITE)
			retcode = !hantro_access_ok(VERIFY_READ, (void *)arg,
						    in_size);
		if (retcode)
			return -EFAULT;
	}
	if (_IOC_TYPE(cmd) == HANTRO_IOC_MAGIC) {
		if (nr >= _IOC_NR(HANTROENC_IOC_START) &&
		    nr <= _IOC_NR(HANTROENC_IOC_END)) {
#ifdef HAS_VCE
			return hantroenc_ioctl(filp, cmd, arg);
#else
			if (cmd == HANTROENC_IOCG_CORE_NUM) {
				int corenum = 0;

				__put_user(corenum, (unsigned int *)arg);
			} else {
				return -EFAULT;
			}
#endif
		}
		if (nr >= _IOC_NR(HANTRODEC_IOC_START) &&
		    nr <= _IOC_NR(HANTRODEC_IOC_END)) {
#ifdef HAS_VCD
			return hantrodec_ioctl(filp, cmd, arg);
#else
			return -EFAULT;
#endif
		}

		if (nr >= _IOC_NR(HANTROCACHE_IOC_START) &&
		    nr <= _IOC_NR(HANTROCACHE_IOC_END)) {
#ifdef HAS_CACHECORE
			return hantrocache_ioctl(filp, cmd, arg);
#else
			return -EFAULT;
#endif
		}

		if (nr >= _IOC_NR(HANTRODEC400_IOC_START) &&
		    nr <= _IOC_NR(HANTRODEC400_IOC_END)) {
#ifdef HAS_DEC400
			return hantrodec400_ioctl(filp, cmd, arg);
#else
			return -EFAULT;
#endif
		}

		if (nr >= _IOC_NR(HANTROMMU_IOC_START) &&
		    nr <= _IOC_NR(HANTROMMU_IOC_END)) {
#ifdef HAS_MMU
			if (hantro_dev.config & CONFIG_HANTROMMU)
				return hantroMMUIoctl(cmd, filp, arg);
			else
				return -EFAULT;
#else
			return -EFAULT;
#endif
		}
#ifdef HAS_VCMD
		if (nr >= _IOC_NR(VCMD_IOC_START) &&
		    nr <= _IOC_NR(VCMD_IOC_END)) {
			int ret;
			mutex_lock(&m_global_lock);
			ret = vcmd_ioctl(filp, cmd, arg);
			mutex_unlock(&m_global_lock);
			return ret;
		}
#endif
	} else if (_IOC_TYPE(cmd) == DRM_IOCTL_BASE) {
		if (nr >= HANTRO_IOCTL_COUNT)
			return -EINVAL;
		ioctl = &hantro_ioctls[nr];

		if (cmd == DRM_IOCTL_HANTRO_UPDATE_METADATA ||
		    cmd == DRM_IOCTL_HANTRO_QUERY_METADATA) {
			if (sizeof(stack_kdata) <
			    sizeof(struct hantro_metadata_params)) {
				pr_err("%s arg size is too large,sizeof(stack_kdata) %ld,in_size %ld\n",
				       __func__, sizeof(stack_kdata),
				       sizeof(struct hantro_metadata_params));
				return -EFAULT;
			}
			if (copy_from_user(
				    kdata, (void __user *)arg,
				    sizeof(struct hantro_metadata_params)) != 0)
				return -EFAULT;
		} else {
			if (sizeof(stack_kdata) < in_size) {
				pr_err("%s arg size is too large,sizeof(stack_kdata) %ld,in_size %d\n",
				       __func__, sizeof(stack_kdata), in_size);
				return -EFAULT;
			}
			if (copy_from_user(kdata, (void __user *)arg,
					   in_size) != 0)
				return -EFAULT;
		}

		if (cmd == DRM_IOCTL_MODE_SETCRTC ||
		    cmd == DRM_IOCTL_MODE_GETRESOURCES ||
		    cmd == DRM_IOCTL_SET_CLIENT_CAP ||
		    cmd == DRM_IOCTL_MODE_GETCRTC ||
		    cmd == DRM_IOCTL_MODE_GETENCODER ||
		    cmd == DRM_IOCTL_MODE_GETCONNECTOR ||
		    cmd == DRM_IOCTL_MODE_GETFB) {
			retcode = drm_ioctl(filp, cmd, arg);
			return retcode;
		}
		func = ioctl->func;
		if (!func)
			return -EINVAL;
		retcode = func(dev, kdata, file_priv);

		if (cmd == DRM_IOCTL_HANTRO_UPDATE_METADATA ||
		    cmd == DRM_IOCTL_HANTRO_QUERY_METADATA) {
			if (copy_to_user(
				    (void __user *)arg, kdata,
				    sizeof(struct hantro_metadata_params)) !=
			    0) {
				retcode = -EFAULT;
			}
		} else {
			if (copy_to_user((void __user *)arg, kdata, out_size) !=
			    0)
				retcode = -EFAULT;
		}
	} else {
		retcode = -EINVAL;
	}

	return retcode;
}

static int hantro_device_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	int vcmd_en;
	hantro_ioctl_id ioctl_id_par;

	ret = drm_open(inode, filp);

	ioctl_id_par.ID_PAR.node_idx = 0;
	vcmd_en = hantro_get_vcmdsup(NULL, &ioctl_id_par.data, NULL);
	//vcmd_en = 0;
	if (vcmd_en == 0) {
#ifdef HAS_VCD
		hantrodec_open(inode, filp);
#endif
#ifdef HAS_CACHECORE
		cache_open(inode, filp);
#endif
	}
#ifdef HAS_VCMD
	else
		hantrovcmd_open(inode, filp);
#endif
#ifdef VSI_CONFIG_PM
	hantro_pm_runtime_get(hantro_dev.drm_dev->dev);
#endif
	return ret;
}

static int hantro_device_release(struct inode *inode, struct file *filp)
{
	int vcmd_en;

	hantro_ioctl_id ioctl_id_par;

	ioctl_id_par.ID_PAR.node_idx = 0;
	vcmd_en = hantro_get_vcmdsup(NULL, &ioctl_id_par.data, NULL);
	//vcmd_en = 0;
	if (vcmd_en == 0) {
#ifdef HAS_CACHECORE
		cache_release(filp);
#endif
#ifdef HAS_VCD
		hantrodec_release(filp);
#endif
#ifdef HAS_VCE
		hantroenc_release();
#endif
#ifdef HAS_MMU
		hantroMMURelease(filp);
#endif
	}
#ifdef HAS_VCMD
	else {
		hantrovcmd_release(inode, filp);
#ifdef HAS_MMU
		hantroMMURelease(filp);
#endif
	}
#endif
#ifdef VSI_CONFIG_PM
	hantro_pm_runtime_put(hantro_dev.drm_dev->dev);
#endif
	return drm_release(inode, filp);
}

static int hantro_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret = 0;
	struct drm_gem_object *obj = NULL;
	struct drm_gem_hantro_object *cma_obj;
	struct drm_vma_offset_node *node;
	unsigned long page_num = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	int sgtidx = 0;
	struct scatterlist *pscatter = NULL;
	struct slice_info *pslice;
	struct device *dev;

	hantro_mmaplog("%s :%lx", __func__, vma->vm_pgoff);
	if (vma->vm_pgoff < VSI_MMAP_ADDRES_CEIL_MMAP)
		return hantro_map_internal_address(filp, vma);
	if (mutex_lock_interruptible(&hantro_dev.struct_mutex))
		return -EBUSY;
	
	vma->vm_pgoff -= VSI_MMAP_ADDRES_CEIL_MMAP;
	
	drm_vma_offset_lock_lookup(hantro_dev.drm_dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(
		hantro_dev.drm_dev->vma_offset_manager, vma->vm_pgoff,
		vma_pages(vma));

	if (likely(node)) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		if (!kref_get_unless_zero(&obj->refcount))
			obj = NULL;
	}
	drm_vma_offset_unlock_lookup(hantro_dev.drm_dev->vma_offset_manager);

	if (!obj) {
		mutex_unlock(&hantro_dev.struct_mutex);
		return -EINVAL;
	}
	hantro_unref_drmobj(obj);
	cma_obj = to_drm_gem_hantro_obj(obj);

	if (page_num > cma_obj->num_pages) {
		mutex_unlock(&hantro_dev.struct_mutex);
		return -EINVAL;
	}
	if (!(cma_obj->flag & HANTRO_GEM_FLAG_IMPORT)) {
		pslice = getslicenode(cma_obj->sliceidx);
		if (!pslice) {
			mutex_unlock(&hantro_dev.struct_mutex);
			return -EINVAL;
		}
		dev = pslice->dev;
	} else {
		dev = obj->dev->dev;
	}
	if ((cma_obj->flag & HANTRO_GEM_FLAG_IMPORT) == 0) {
		if (cma_obj->vaddr == 0) {
			mutex_unlock(&hantro_dev.struct_mutex);
			return -EINVAL;
		}
		ret = hantro_drm_gem_mmap_obj(
			obj, drm_vma_node_size(node) << PAGE_SHIFT, vma);

		if (ret) {
			mutex_unlock(&hantro_dev.struct_mutex);
			return ret;
		}
	} else {
		pscatter = &cma_obj->sgt->sgl[sgtidx];
#ifdef __amd64__
		set_memory_uc((unsigned long)cma_obj->vaddr, (int)page_num);
#endif
#ifndef USE_CMA
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#endif
		/*else mmap report uncached error for some importer, e.g. i915*/
	}

	if (cma_obj->flag & HANTRO_GEM_FLAG_USEVMALLOC) {
		int i, j;
		unsigned long uaddr = vma->vm_start;
		void *address = cma_obj->vaddr;
		struct page *pages = NULL;

		lg_vm_flags_set(vma, (vma->vm_flags | VM_LOCKED) & ~VM_PFNMAP);

		for (i = 0; i < page_num; i++) {
			pages = (vmalloc_to_page(address));
			if (IS_ERR(pages) || !page_count(pages) ||
			    vm_insert_page(vma, uaddr, pages)) {
				pr_err("alloc page %d fail = %lx:%lx\n", i,
				       (unsigned long)address,
				       (unsigned long)pages);
				address = cma_obj->vaddr;
				for (j = 0; j < i; j++) {
					pages = (vmalloc_to_page(address));
					unref_page(pages);
					address += PAGE_SIZE;
				}
				mutex_unlock(&hantro_dev.struct_mutex);
				return -ENOMEM;
			}
			ref_page(pages);
			uaddr += PAGE_SIZE;
			address += PAGE_SIZE;
		}
	} else {
#ifndef VSI_FPGA_MEM
		vma->vm_pgoff = 0;
		if (dma_mmap_coherent(dev, vma, cma_obj->vaddr, cma_obj->paddr,
				      page_num << PAGE_SHIFT)) {
			mutex_unlock(&hantro_dev.struct_mutex);
			return -EAGAIN;
		}
#else
		remap_pfn_range(vma, vma->vm_start,
				cma_obj->paddr >> PAGE_SHIFT,
				(vma->vm_end - vma->vm_start), vma->vm_page_prot);
#endif
	}
	vma->vm_private_data = cma_obj;
	mutex_unlock(&hantro_dev.struct_mutex);
	return ret;
}

/* VFS methods */
const struct file_operations hantro_fops = {
	.owner = THIS_MODULE,
	.open = hantro_device_open,
	.mmap = hantro_mmap,
	.release = hantro_device_release,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = hantro_ioctl, //drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
#ifdef FOP_UNSIGNED_OFFSET
	.fop_flags = FOP_UNSIGNED_OFFSET,
#endif
};

static void hantro_gem_vm_close(struct vm_area_struct *vma)
{
	drm_gem_vm_close(vma);
}

static void hantro_release(struct drm_device *dev)
{
	drm_dev_unregister(hantro_dev.drm_dev);
#if defined(LG_DRM_DEV_FINI)
	drm_dev_fini(hantro_dev.drm_dev);
#else
	drm_dev_put(hantro_dev.drm_dev);
#endif
}

static int hantro_gem_prime_handle_to_fd(struct drm_device *dev,
					 struct drm_file *filp, uint32_t handle,
					 u32 flags, int *prime_fd)
{
	return drm_gem_prime_handle_to_fd(dev, filp, handle, flags, prime_fd);
}

#if defined(LG_VM_OPERATIONS_FAULT_RET_INT)
static int hantro_vm_fault(struct vm_fault *vmf)
{
	return -EPERM;
}
#else
static vm_fault_t hantro_vm_fault(struct vm_fault *vmf)
{
	return -EPERM;
}
#endif

static const struct vm_operations_struct hantro_drm_gem_cma_vm_ops = {
	.open = drm_gem_vm_open,
	.close = hantro_gem_vm_close,
	.fault = hantro_vm_fault,
};

/*temp no usage now*/
static u32 hantro_vblank_no_hw_counter(struct drm_device *dev,
				       unsigned int pipe)
{
	return 0;
}


#ifdef HAS_MMU
// Demo. Customer maybe need change it accrording to own environment
static void FreeMemWithMMU(struct drm_gem_hantro_object *cma_obj)
{
	vfree(cma_obj->vaddr);
}

// Demo. Customer maybe need change it accrording to own environment
static int AllocMemWithMMU(struct drm_gem_hantro_object *cma_obj,  struct drm_mode_create_dumb *args)
{
	cma_obj->vaddr = vmalloc(args->size);
	if (!cma_obj->vaddr)
		return -ENOMEM;
	cma_obj->paddr = page_to_phys(vmalloc_to_page(cma_obj->vaddr));
	cma_obj->flag |= HANTRO_GEM_FLAG_USEVMALLOC;
	return 0;
}
#else
#ifdef USE_CMA
static void FreeCMAMem(struct drm_gem_hantro_object *cma_obj)
{
	struct slice_info *pslice = getslicenode(cma_obj->sliceidx);

	if (!pslice)
		return;
	dma_free_wc(pslice->dev, cma_obj->num_pages * PAGE_SIZE,
            cma_obj->vaddr, cma_obj->paddr);
}

/*For X86 environment, CMA maybe need disabled for these reasons:
 *1. to bring up CMA, CONFIG_DMA_CMA should be enabled in kernel building
 *2. dma_alloc/release_from_contiguous should be exported in kernel code
 *3. a boot parameter like "cma=268435456@134217728" should be added
 *4. CMA's memory management is not stable.
 */

static int AllocateCMAMem(struct drm_gem_hantro_object *cma_obj, struct drm_mode_create_dumb *args)
{
	struct slice_info *pslice = getslicenode(args->handle);

	cma_obj->vaddr = dma_alloc_wc(pslice->dev, args->size,
                    &cma_obj->paddr, GFP_KERNEL);

	if (!cma_obj->vaddr)
		return -ENOMEM;

	return 0;
}
#else
static void FreeDMAMem(struct drm_gem_hantro_object *cma_obj)
{
	struct slice_info *pslice = getslicenode(cma_obj->sliceidx);

	if (!pslice)
		return;
	dma_free_coherent(pslice->dev, cma_obj->base.size,
				cma_obj->vaddr, cma_obj->paddr);
}

static int AllocateDMAMem(struct drm_gem_hantro_object *cma_obj, struct drm_mode_create_dumb *args)
{
	struct slice_info *pslice = getslicenode(args->handle);

	cma_obj->vaddr = dma_alloc_coherent(pslice->dev, args->size,
						&cma_obj->paddr,
						GFP_KERNEL | GFP_DMA);
	if (!cma_obj->vaddr)
		return -ENOMEM;
	return 0;
}
#endif
#endif

#ifdef VSI_FPGA_MEM
static void FreeHantroFpgaMem(struct drm_gem_hantro_object *cma_obj)
{
	struct hantro_mem_handle fpga_phandle;

	fpga_phandle.vaddr = cma_obj->vaddr;
	fpga_phandle.paddr = cma_obj->paddr;
	hantro_fpga_memfree(&fpga_phandle);
}

static int AllocHantroFpgaMem(struct drm_gem_hantro_object *cma_obj, struct drm_mode_create_dumb *args)
{
	struct hantro_mem_handle fpga_phandle;
	int ret;

	ret = hantro_fpga_memalloc(&fpga_phandle, args->size);
	if (ret != 0) {
		pr_err("hantro_fpga_memalloc ret error\n");
		return ret;
	}
	cma_obj->vaddr = fpga_phandle.vaddr;
	cma_obj->paddr = fpga_phandle.paddr;
	cma_obj->mem_base = fpga_phandle.mem_base;
	return 0;
}
#endif

static void FreeMem(struct drm_gem_hantro_object *cma_obj)
{
	int ret;

	ret = memory_pool_free((unsigned long)cma_obj->vaddr);
	if (!ret)
		return;

#ifdef VSI_FPGA_MEM
	FreeHantroFpgaMem(cma_obj);
#else

//Customer changeing following implement based on own environment
#ifdef HAS_MMU
	FreeMemWithMMU(cma_obj);
#else

#ifdef USE_CMA
	FreeCMAMem(cma_obj);
#else
	FreeDMAMem(cma_obj);
#endif

#endif//HAS_MMU end

#endif//VSI_FPGA_MEM end
}

static int AllocateMem(struct drm_gem_hantro_object *cma_obj, struct drm_mode_create_dumb *args)
{
	int ret;

	ret = memory_pool_alloc((unsigned long *)&cma_obj->vaddr,
			(unsigned long *)&cma_obj->paddr, args->size);
	if (!ret)
		return ret;

#ifdef VSI_FPGA_MEM
	ret = AllocHantroFpgaMem(cma_obj, args);
#else

//Customer changeing following implement based on own environment
#ifdef HAS_MMU
	ret = AllocMemWithMMU(cma_obj, args);
#else

#ifdef USE_CMA
	ret = AllocateCMAMem(cma_obj, args);
#else
	ret = AllocateDMAMem(cma_obj, args);
#endif

#endif

#endif
	return ret;
}

#if defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
static const struct drm_gem_object_funcs hantro_drm_gem_cma_funcs = {
	.export = hantro_prime_export,
	.get_sg_table = hantro_gem_prime_get_sg_table,
	.vmap = hantro_gem_prime_vmap,
	.vunmap = hantro_gem_prime_vunmap,
	.free = hantro_gem_free_object,
	.vm_ops = &hantro_drm_gem_cma_vm_ops,
};
#endif

struct drm_driver hantro_drm_driver = {
	//these two are related with controlD and renderD
	.driver_features = DRIVER_GEM | DRIVER_RENDER
#if defined(LG_DRM_DRIVER_PRIME_FLAG_PRESENT)
	 | DRIVER_PRIME
#endif
	,
#ifdef CONFIG_DRM_LEGACY
	.get_vblank_counter = hantro_vblank_no_hw_counter,
#endif
	.open = hantro_drm_open,
	.release = hantro_release,
	.dumb_create = hantro_gem_dumb_create_internal,
	.dumb_map_offset = hantro_gem_dumb_map_offset,
#if defined(LG_DRM_DRIVER_HAS_EXPORT)
#if defined(LG_GEM_PRIME_EXPORT_HAS_DRM_DEVICE)
	.gem_prime_export = drm_gem_prime_export,
#else
	.gem_prime_export = hantro_prime_export,
#endif
	.gem_vm_ops = &hantro_drm_gem_cma_vm_ops,
	.gem_free_object_unlocked = hantro_gem_free_object,
#if !defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
	.gem_prime_get_sg_table = hantro_gem_prime_get_sg_table,
	.gem_prime_vmap = hantro_gem_prime_vmap,
	.gem_prime_vunmap = hantro_gem_prime_vunmap,
#endif
#endif

#if defined(LG_DRM_DRIVER_HAS_GEM_DUMB_DESTROY)
	.dumb_destroy = hantro_gem_dumb_destroy,
#endif
	.gem_prime_import = hantro_drm_gem_prime_import,
	.prime_handle_to_fd = hantro_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import_sg_table = hantro_gem_prime_import_sg_table,
	//.gem_prime_res_obj = hantro_gem_prime_res_obj,
#if defined(LG_DRM_DRIVER_HAS_GEM_PRIME_MMAP)
	//.gem_prime_mmap = hantro_gem_prime_mmap,
#endif
	.fops = &hantro_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0))
	.date = DRIVER_DATE,
#endif
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};


static int hantro_drm_gem_mmap_obj(struct drm_gem_object *obj, unsigned long obj_size,
				 struct vm_area_struct *vma)
{
	struct drm_device __maybe_unused *dev = obj->dev;

	/* Check for valid size. */
	if (obj_size < vma->vm_end - vma->vm_start)
		return -EINVAL;

#if !defined(LG_DRM_GEM_OBJECT_HAS_FUNCS)
	if (!dev->driver->gem_vm_ops)
		return -EINVAL;

	lg_vm_flags_set(vma, vma->vm_flags | VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	vma->vm_ops = dev->driver->gem_vm_ops;
	vma->vm_private_data = obj;
#ifndef USE_CMA
	vma->vm_page_prot = pgprot_noncached(vm_get_page_prot(vma->vm_flags));
#else
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
#endif

	/* Take a ref for this mapping of the object, so that the fault
	 * handler can dereference the mmap offset's pointer to the object.
	 * This reference is cleaned up by the corresponding vm_close
	 * (which should happen whether the vma was created by this call, or
	 * by a vm_open due to mremap or partial unmap or whatever).
	 */
	drm_gem_object_get(obj);
	return 0;
#else
	int ret;
	drm_gem_object_get(obj);
	vma->vm_private_data = obj;
	vma->vm_ops = obj->funcs->vm_ops;
	if (!vma->vm_ops) {
		ret = -EINVAL;
		goto err_drm_gem_object_put;
	}
	lg_vm_flags_set(vma, vma->vm_flags | VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
#ifndef USE_CMA
	vma->vm_page_prot = pgprot_noncached(vm_get_page_prot(vma->vm_flags));
#else
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
#endif
	return 0;

err_drm_gem_object_put:
	drm_gem_object_put(obj);
	return ret;


#endif
}

#if 0
struct drm_device *create_hantro_drm(struct device *dev)
{
	struct drm_device *ddev;
	int result;

	ddev = drm_dev_alloc(&hantro_drm_driver, dev);
	if (IS_ERR(ddev))
		return ddev;

	ddev->dev = dev;
	drm_mode_config_init(ddev);
	result = drm_dev_register(ddev, 0);
	if (result < 0) {
		drm_dev_unregister(ddev);
		drm_dev_put(ddev);
		return NULL;
	}

	return ddev;
}
#endif






