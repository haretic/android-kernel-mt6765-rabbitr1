// SPDX-License-Identifier: GPL-2.0
/*
 * Legacy MediaTek ION compatibility device.
 *
 * Stock MediaTek Android vendor blobs (PowerVR gralloc, libion_mtk,
 * libgralloc_extra) talk to /dev/ion with the handle-based ION ABI of the
 * 4.19 vendor kernel plus MediaTek's ION_IOC_CUSTOM extensions. This driver
 * implements that ABI on top of plain DMA-BUF: every allocation, from any
 * heap id the blobs ask for, is a page-backed dma-buf exported here, so the
 * buffers can be imported by any mainline driver (mediatek-drm PRIME,
 * PowerVR, V4L2, ...).
 *
 * Handles are per open file, as in legacy ION: ALLOC and IMPORT return a
 * handle, SHARE/MAP turn a handle into a dma-buf fd, FREE drops one
 * reference on the handle. IMPORT of a buffer that already has a handle in
 * the same client returns the same handle with an extra reference.
 *
 * The page allocator follows drivers/dma-buf/heaps/system_heap.c, with the
 * legacy ION_FLAG_CACHED semantic added back: buffers without the flag are
 * flushed once at allocation and mapped write-combined for the CPU.
 */

#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include <uapi/linux/dma-buf.h>

#include "mtk_ion_abi.h"

#define MIC_EXP_NAME	"mtk_ion_compat"

static_assert(sizeof(struct ion_allocation_data) == 32);
static_assert(sizeof(struct ion_custom_data) == 16);
static_assert(sizeof(struct ion_sys_data) == 120);
static_assert(sizeof(struct ion_mm_data) == 80);
static_assert(sizeof(struct ion_heap_query) == 24);
static_assert(sizeof(struct ion_heap_data) == 52);

/* Heaps with physically contiguous memory in the stock driver set. */
#define MIC_CONTIG_HEAPS	(BIT(MTK_ION_HEAP_SYSTEM_CONTIG) | \
				 BIT(MTK_ION_HEAP_CARVEOUT) | \
				 BIT(MTK_ION_HEAP_DMA))
/* Secure heaps: no TEE here, so they are ordinary (non-secure) memory. */
#define MIC_SECURE_HEAPS	(BIT(MTK_ION_HEAP_MULTIMEDIA_SEC) | \
				 GENMASK(MTK_ION_HEAP_MULTIMEDIA_SDSP_SHARED, \
					 MTK_ION_HEAP_MULTIMEDIA_PROT))

struct mic_heap_desc {
	unsigned int id;
	unsigned int type;
	const char *name;
};

/* Heap table of the stock MT6765 kernel (ion_drv_platform_heaps). */
static const struct mic_heap_desc mic_heaps[] = {
	{ MTK_ION_HEAP_SYSTEM, 0, "ion_system_heap" },
	{ MTK_ION_HEAP_MULTIMEDIA, 10, "ion_mm_heap" },
	{ MTK_ION_HEAP_MULTIMEDIA_FOR_CAMERA, 10, "ion_mm_heap_for_camera" },
	{ MTK_ION_HEAP_MULTIMEDIA_SEC, 13, "ion_sec_heap" },
	{ MTK_ION_HEAP_MULTIMEDIA_PROT, 13, "ion_sec_heap_protected" },
	{ MTK_ION_HEAP_MULTIMEDIA_2D_FR, 13, "ion_sec_heap_2d_fr" },
	{ MTK_ION_HEAP_MULTIMEDIA_WFD, 13, "ion_sec_heap_wfd" },
	{ MTK_ION_HEAP_MULTIMEDIA_HAPP, 13, "ion_sec_heap_happ" },
	{ MTK_ION_HEAP_MULTIMEDIA_HAPP_EXTRA, 13, "ion_sec_heap_happ_mem" },
	{ MTK_ION_HEAP_MULTIMEDIA_SDSP, 13, "ion_sec_heap_sdsp" },
	{ MTK_ION_HEAP_MULTIMEDIA_SDSP_SHARED, 13, "ion_sec_heap_sdsp_shared" },
	{ MTK_ION_HEAP_CARVEOUT, 2, "ion_carveout_heap" },
};

static struct miscdevice mic_misc;

struct mic_buffer {
	struct list_head attachments;
	struct mutex lock;		/* attachments, vmap, metadata */
	size_t len;
	struct sg_table sgt;
	bool cached;
	bool contig;
	int vmap_cnt;
	void *vaddr;

	/* MediaTek per-buffer metadata (ION_MM_* custom commands). */
	unsigned int heap_id_mask;
	unsigned int flags;
	int module_id;
	unsigned int security;
	unsigned int coherent;
	char dbg_name[ION_MM_DBG_NAME_LEN];
	unsigned int dbg_value[4];
};

struct mic_attachment {
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	bool mapped;
};

struct mic_handle {
	struct dma_buf *dmabuf;
	unsigned int refs;
};

struct mic_client {
	struct mutex lock;		/* handles */
	struct xarray handles;
	char name[ION_MM_DBG_NAME_LEN];
	pid_t pid;
};

static const struct dma_buf_ops mic_dmabuf_ops;

static bool mic_is_ours(struct dma_buf *dmabuf)
{
	return dmabuf->ops == &mic_dmabuf_ops;
}

/* ------------------------------------------------------------------------
 * DMA-BUF exporter
 */

static int mic_dup_sg_table(struct sg_table *from, struct sg_table *to)
{
	struct scatterlist *sg, *new_sg;
	int ret, i;

	ret = sg_alloc_table(to, from->orig_nents, GFP_KERNEL);
	if (ret)
		return ret;

	new_sg = to->sgl;
	for_each_sgtable_sg(from, sg, i) {
		sg_set_page(new_sg, sg_page(sg), sg->length, sg->offset);
		new_sg = sg_next(new_sg);
	}

	return 0;
}

static int mic_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *att)
{
	struct mic_buffer *buffer = dmabuf->priv;
	struct mic_attachment *a;
	int ret;

	a = kzalloc_obj(*a);
	if (!a)
		return -ENOMEM;

	ret = mic_dup_sg_table(&buffer->sgt, &a->table);
	if (ret) {
		kfree(a);
		return ret;
	}

	a->dev = att->dev;
	INIT_LIST_HEAD(&a->list);
	att->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void mic_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *att)
{
	struct mic_buffer *buffer = dmabuf->priv;
	struct mic_attachment *a = att->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *mic_map_dma_buf(struct dma_buf_attachment *att,
					enum dma_data_direction dir)
{
	struct mic_buffer *buffer = att->dmabuf->priv;
	struct mic_attachment *a = att->priv;
	unsigned long attrs = buffer->cached ? 0 : DMA_ATTR_SKIP_CPU_SYNC;
	int ret;

	ret = dma_map_sgtable(att->dev, &a->table, dir, attrs);
	if (ret)
		return ERR_PTR(ret);

	a->mapped = true;
	return &a->table;
}

static void mic_unmap_dma_buf(struct dma_buf_attachment *att,
			      struct sg_table *table,
			      enum dma_data_direction dir)
{
	struct mic_buffer *buffer = att->dmabuf->priv;
	struct mic_attachment *a = att->priv;
	unsigned long attrs = buffer->cached ? 0 : DMA_ATTR_SKIP_CPU_SYNC;

	a->mapped = false;
	dma_unmap_sgtable(att->dev, table, dir, attrs);
}

static int mic_begin_cpu_access(struct dma_buf *dmabuf,
				enum dma_data_direction dir)
{
	struct mic_buffer *buffer = dmabuf->priv;
	struct mic_attachment *a;

	/* Uncached buffers are only ever mapped write-combined. */
	if (!buffer->cached)
		return 0;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt)
		invalidate_kernel_vmap_range(buffer->vaddr, buffer->len);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (a->mapped)
			dma_sync_sgtable_for_cpu(a->dev, &a->table, dir);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int mic_end_cpu_access(struct dma_buf *dmabuf,
			      enum dma_data_direction dir)
{
	struct mic_buffer *buffer = dmabuf->priv;
	struct mic_attachment *a;

	if (!buffer->cached)
		return 0;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt)
		flush_kernel_vmap_range(buffer->vaddr, buffer->len);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (a->mapped)
			dma_sync_sgtable_for_device(a->dev, &a->table, dir);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int mic_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct mic_buffer *buffer = dmabuf->priv;
	unsigned long addr = vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;
	struct scatterlist *sg;
	pgprot_t prot;
	int i, ret;

	prot = vma->vm_page_prot;
	if (!buffer->cached)
		prot = pgprot_writecombine(prot);
	vma->vm_page_prot = prot;

	for_each_sgtable_sg(&buffer->sgt, sg, i) {
		unsigned long n = sg->length >> PAGE_SHIFT;

		if (pgoff < n)
			break;
		pgoff -= n;
	}

	for (; sg && addr < vma->vm_end; sg = sg_next(sg)) {
		unsigned long n = (sg->length >> PAGE_SHIFT) - pgoff;
		struct page *page = sg_page(sg) + pgoff;
		unsigned long size = n << PAGE_SHIFT;

		if (addr + size > vma->vm_end)
			size = vma->vm_end - addr;

		ret = remap_pfn_range(vma, addr, page_to_pfn(page), size, prot);
		if (ret)
			return ret;

		addr += size;
		pgoff = 0;
	}

	return 0;
}

static void *mic_do_vmap(struct mic_buffer *buffer)
{
	unsigned int npages = buffer->len >> PAGE_SHIFT;
	struct sg_page_iter piter;
	struct page **pages, **tmp;
	void *vaddr;

	pages = vmalloc_array(npages, sizeof(*pages));
	if (!pages)
		return ERR_PTR(-ENOMEM);

	tmp = pages;
	for_each_sgtable_page(&buffer->sgt, &piter, 0) {
		if (WARN_ON(tmp - pages >= npages))
			break;
		*tmp++ = sg_page_iter_page(&piter);
	}

	vaddr = vmap(pages, npages, VM_MAP,
		     buffer->cached ? PAGE_KERNEL :
				      pgprot_writecombine(PAGE_KERNEL));
	vfree(pages);

	return vaddr ? vaddr : ERR_PTR(-ENOMEM);
}

static int mic_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct mic_buffer *buffer = dmabuf->priv;
	void *vaddr;
	int ret = 0;

	mutex_lock(&buffer->lock);
	if (!buffer->vmap_cnt) {
		vaddr = mic_do_vmap(buffer);
		if (IS_ERR(vaddr)) {
			ret = PTR_ERR(vaddr);
			goto out;
		}
		buffer->vaddr = vaddr;
	}
	buffer->vmap_cnt++;
	iosys_map_set_vaddr(map, buffer->vaddr);
out:
	mutex_unlock(&buffer->lock);
	return ret;
}

static void mic_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct mic_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	if (!--buffer->vmap_cnt) {
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}
	mutex_unlock(&buffer->lock);
	iosys_map_clear(map);
}

static void mic_free_pages(struct mic_buffer *buffer)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(&buffer->sgt, sg, i) {
		struct page *page = sg_page(sg);

		if (buffer->contig)
			free_pages_exact(page_address(page), sg->length);
		else
			__free_pages(page, compound_order(page));
	}
	sg_free_table(&buffer->sgt);
}

static void mic_release(struct dma_buf *dmabuf)
{
	struct mic_buffer *buffer = dmabuf->priv;

	mic_free_pages(buffer);
	kfree(buffer);
}

static const struct dma_buf_ops mic_dmabuf_ops = {
	.attach = mic_attach,
	.detach = mic_detach,
	.map_dma_buf = mic_map_dma_buf,
	.unmap_dma_buf = mic_unmap_dma_buf,
	.begin_cpu_access = mic_begin_cpu_access,
	.end_cpu_access = mic_end_cpu_access,
	.mmap = mic_mmap,
	.vmap = mic_vmap,
	.vunmap = mic_vunmap,
	.release = mic_release,
};

#define MIC_LOW_ORDER_GFP	(GFP_HIGHUSER | __GFP_ZERO)
#define MIC_HIGH_ORDER_GFP	(((GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN | \
				   __GFP_NORETRY) & ~__GFP_RECLAIM) | \
				 __GFP_COMP)
static const gfp_t mic_order_flags[] = {
	MIC_HIGH_ORDER_GFP, MIC_HIGH_ORDER_GFP, MIC_LOW_ORDER_GFP
};

static const unsigned int mic_orders[] = { 8, 4, 0 };

static struct page *mic_alloc_largest(unsigned long size, unsigned int max)
{
	struct page *page;
	int i;

	for (i = 0; i < ARRAY_SIZE(mic_orders); i++) {
		if (size < (PAGE_SIZE << mic_orders[i]))
			continue;
		if (max < mic_orders[i])
			continue;
		page = alloc_pages(mic_order_flags[i], mic_orders[i]);
		if (page)
			return page;
	}
	return NULL;
}

static int mic_alloc_scattered(struct mic_buffer *buffer)
{
	unsigned long remaining = buffer->len;
	unsigned int max_order = mic_orders[0];
	struct page *page, *tmp;
	struct scatterlist *sg;
	LIST_HEAD(pages);
	int n = 0, ret = -ENOMEM;

	while (remaining) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto err;
		}
		page = mic_alloc_largest(remaining, max_order);
		if (!page)
			goto err;
		list_add_tail(&page->lru, &pages);
		remaining -= page_size(page);
		max_order = compound_order(page);
		n++;
	}

	ret = sg_alloc_table(&buffer->sgt, n, GFP_KERNEL);
	if (ret)
		goto err;

	sg = buffer->sgt.sgl;
	list_for_each_entry_safe(page, tmp, &pages, lru) {
		sg_set_page(sg, page, page_size(page), 0);
		sg = sg_next(sg);
		list_del(&page->lru);
	}
	return 0;

err:
	list_for_each_entry_safe(page, tmp, &pages, lru)
		__free_pages(page, compound_order(page));
	return ret;
}

static int mic_alloc_contig(struct mic_buffer *buffer)
{
	void *vaddr;
	int ret;

	if (get_order(buffer->len) > MAX_PAGE_ORDER)
		return -ENOMEM;

	vaddr = alloc_pages_exact(buffer->len, GFP_KERNEL | __GFP_ZERO |
					       __GFP_NOWARN);
	if (!vaddr)
		return -ENOMEM;

	ret = sg_alloc_table(&buffer->sgt, 1, GFP_KERNEL);
	if (ret) {
		free_pages_exact(vaddr, buffer->len);
		return ret;
	}
	sg_set_page(buffer->sgt.sgl, virt_to_page(vaddr), buffer->len, 0);
	buffer->contig = true;
	return 0;
}

static const char *mic_heap_name(unsigned int heap_id_mask)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mic_heaps); i++)
		if (heap_id_mask & BIT(mic_heaps[i].id))
			return mic_heaps[i].name;
	return mic_heaps[0].name;
}

static void mic_set_dmabuf_name(struct dma_buf *dmabuf, const char *name)
{
	char *copy = kstrndup(name, DMA_BUF_NAME_LEN - 1, GFP_KERNEL);

	if (!copy)
		return;
	spin_lock(&dmabuf->name_lock);
	kfree(dmabuf->name);
	dmabuf->name = copy;
	spin_unlock(&dmabuf->name_lock);
}

static struct dma_buf *mic_buffer_alloc(size_t len, unsigned int heap_id_mask,
					unsigned int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct mic_buffer *buffer;
	struct dma_buf *dmabuf;
	int ret;

	if (!len || len > SIZE_MAX - PAGE_SIZE)
		return ERR_PTR(-EINVAL);
	len = PAGE_ALIGN(len);
	if ((len >> PAGE_SHIFT) > totalram_pages())
		return ERR_PTR(-ENOMEM);
	if (!heap_id_mask)
		return ERR_PTR(-ENODEV);

	if (heap_id_mask & MIC_SECURE_HEAPS)
		pr_info_once("mtk_ion_compat: secure heap request (mask %#x) served from normal memory\n",
			     heap_id_mask);

	buffer = kzalloc_obj(*buffer);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->len = len;
	buffer->cached = flags & ION_FLAG_CACHED;
	buffer->heap_id_mask = heap_id_mask;
	buffer->flags = flags;
	buffer->module_id = -1;

	if (!(heap_id_mask & ~MIC_CONTIG_HEAPS))
		ret = mic_alloc_contig(buffer);
	else
		ret = mic_alloc_scattered(buffer);
	if (ret)
		goto err_free_buffer;

	/*
	 * Write the zeroed pages back to memory once, so non-coherent devices
	 * and write-combined CPU mappings never see stale cache lines.
	 */
	ret = dma_map_sgtable(mic_misc.this_device, &buffer->sgt,
			      DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_pages;
	dma_unmap_sgtable(mic_misc.this_device, &buffer->sgt, DMA_BIDIRECTIONAL,
			  DMA_ATTR_SKIP_CPU_SYNC);

	exp_info.exp_name = MIC_EXP_NAME;
	exp_info.ops = &mic_dmabuf_ops;
	exp_info.size = len;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_pages;
	}

	mic_set_dmabuf_name(dmabuf, mic_heap_name(heap_id_mask));
	return dmabuf;

err_free_pages:
	mic_free_pages(buffer);
err_free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------------
 * Handles
 */

/* Takes over the caller's dma-buf reference on success. */
static int mic_handle_create(struct mic_client *client, struct dma_buf *dmabuf,
			     u32 *id)
{
	struct mic_handle *handle;
	int ret;

	handle = kzalloc_obj(*handle);
	if (!handle)
		return -ENOMEM;
	handle->dmabuf = dmabuf;
	handle->refs = 1;

	ret = xa_alloc(&client->handles, id, handle, XA_LIMIT(1, INT_MAX),
		       GFP_KERNEL);
	if (ret)
		kfree(handle);
	return ret;
}

/* Drops one user reference; returns the dma-buf to put, if any. */
static struct dma_buf *mic_handle_put_locked(struct mic_client *client,
					     u32 id)
{
	struct mic_handle *handle = xa_load(&client->handles, id);
	struct dma_buf *dmabuf;

	if (!handle)
		return ERR_PTR(-EINVAL);
	if (--handle->refs)
		return NULL;

	xa_erase(&client->handles, id);
	dmabuf = handle->dmabuf;
	kfree(handle);
	return dmabuf;
}

static int mic_handle_free(struct mic_client *client, int id)
{
	struct dma_buf *dmabuf;

	if (id <= 0)
		return -EINVAL;

	mutex_lock(&client->lock);
	dmabuf = mic_handle_put_locked(client, id);
	mutex_unlock(&client->lock);

	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	if (dmabuf)
		dma_buf_put(dmabuf);
	return 0;
}

/* Returns the handle's dma-buf with an extra reference. */
static struct dma_buf *mic_handle_get_dmabuf(struct mic_client *client, int id)
{
	struct mic_handle *handle;
	struct dma_buf *dmabuf = ERR_PTR(-EINVAL);

	if (id <= 0)
		return dmabuf;

	mutex_lock(&client->lock);
	handle = xa_load(&client->handles, id);
	if (handle) {
		dmabuf = handle->dmabuf;
		get_dma_buf(dmabuf);
	}
	mutex_unlock(&client->lock);
	return dmabuf;
}

static int mic_ioc_alloc(struct mic_client *client, size_t len,
			 unsigned int heap_id_mask, unsigned int flags,
			 int *id)
{
	struct dma_buf *dmabuf;
	u32 new_id;
	int ret;

	dmabuf = mic_buffer_alloc(len, heap_id_mask, flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	mutex_lock(&client->lock);
	ret = mic_handle_create(client, dmabuf, &new_id);
	mutex_unlock(&client->lock);
	if (ret) {
		dma_buf_put(dmabuf);
		return ret;
	}

	*id = new_id;
	return 0;
}

static int mic_ioc_share(struct mic_client *client, int id)
{
	struct dma_buf *dmabuf;
	int fd;

	dmabuf = mic_handle_get_dmabuf(client, id);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);
	return fd;
}

static int mic_ioc_import(struct mic_client *client, int fd, int *id)
{
	struct mic_handle *handle;
	struct dma_buf *dmabuf;
	unsigned long idx;
	u32 new_id;
	int ret = 0;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	mutex_lock(&client->lock);
	xa_for_each(&client->handles, idx, handle) {
		if (handle->dmabuf != dmabuf)
			continue;
		if (handle->refs == UINT_MAX) {
			ret = -EOVERFLOW;
		} else {
			handle->refs++;
			*id = idx;
		}
		mutex_unlock(&client->lock);
		dma_buf_put(dmabuf);
		return ret;
	}

	ret = mic_handle_create(client, dmabuf, &new_id);
	mutex_unlock(&client->lock);
	if (ret) {
		dma_buf_put(dmabuf);
		return ret;
	}
	*id = new_id;
	return 0;
}

static int mic_ioc_sync(int fd)
{
	struct dma_buf *dmabuf;
	int ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* Legacy ION_IOC_SYNC is a sync-for-device of the whole buffer. */
	ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	dma_buf_put(dmabuf);
	return ret;
}

static int mic_ioc_heap_query(struct ion_heap_query *query)
{
	struct ion_heap_data __user *uheaps = u64_to_user_ptr(query->heaps);
	struct ion_heap_data hdata;
	u32 i, max_cnt;

	if (query->reserved0 || query->reserved1 || query->reserved2)
		return -EINVAL;

	if (!uheaps) {
		query->cnt = ARRAY_SIZE(mic_heaps);
		return 0;
	}

	max_cnt = query->cnt;
	if (!max_cnt)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mic_heaps) && i < max_cnt; i++) {
		memset(&hdata, 0, sizeof(hdata));
		strscpy(hdata.name, mic_heaps[i].name, sizeof(hdata.name));
		hdata.type = mic_heaps[i].type;
		hdata.heap_id = mic_heaps[i].id;
		if (copy_to_user(&uheaps[i], &hdata, sizeof(hdata)))
			return -EFAULT;
	}
	query->cnt = i;
	return 0;
}

/* ------------------------------------------------------------------------
 * ION_IOC_CUSTOM: ION_CMD_SYSTEM
 */

static int mic_cache_sync(struct dma_buf *dmabuf, unsigned int sync_type)
{
	int ret = 0;

	switch (sync_type) {
	case ION_CACHE_CLEAN_BY_RANGE:
	case ION_CACHE_CLEAN_BY_RANGE_USE_PA:
	case ION_CACHE_CLEAN_ALL:
		ret = dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
		break;
	case ION_CACHE_INVALID_BY_RANGE:
	case ION_CACHE_INVALID_BY_RANGE_USE_PA:
	case ION_CACHE_INVALID_ALL:
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		break;
	case ION_CACHE_FLUSH_BY_RANGE:
	case ION_CACHE_FLUSH_BY_RANGE_USE_PA:
	case ION_CACHE_FLUSH_ALL:
		ret = dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		if (!ret)
			ret = dma_buf_begin_cpu_access(dmabuf,
						       DMA_BIDIRECTIONAL);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

/* Physical address of a buffer, if it is one contiguous chunk. */
static int mic_buffer_phys(struct dma_buf *dmabuf, u64 *phys, u64 *len)
{
	struct mic_buffer *buffer;

	if (!mic_is_ours(dmabuf))
		return -EFAULT;
	buffer = dmabuf->priv;
	if (buffer->sgt.orig_nents != 1)
		return -EFAULT;

	*phys = sg_phys(buffer->sgt.sgl);
	*len = buffer->len;
	return 0;
}

static long mic_sys_do(struct mic_client *client, struct ion_sys_data *p)
{
	struct dma_buf *dmabuf;
	u64 phys, len;
	long ret;

	switch (p->sys_cmd) {
	case ION_SYS_CACHE_SYNC:
		dmabuf = mic_handle_get_dmabuf(client,
					       p->cache_sync_param.handle);
		if (IS_ERR(dmabuf))
			return -EINVAL;
		ret = mic_cache_sync(dmabuf, p->cache_sync_param.sync_type);
		dma_buf_put(dmabuf);
		return ret;

	case ION_SYS_GET_PHYS:
		dmabuf = mic_handle_get_dmabuf(client,
					       p->get_phys_param.handle);
		if (IS_ERR(dmabuf))
			return -EINVAL;
		ret = mic_buffer_phys(dmabuf, &phys, &len);
		dma_buf_put(dmabuf);
		if (ret) {
			pr_debug("mtk_ion_compat: %s: GET_PHYS on a scattered buffer\n",
				 client->name);
			p->get_phys_param.phy_addr = 0;
			p->get_phys_param.len = 0;
			return ret;
		}
		p->get_phys_param.phy_addr = phys;
		p->get_phys_param.len = len;
		return 0;

	case ION_SYS_SET_CLIENT_NAME:
		mutex_lock(&client->lock);
		memcpy(client->name, p->client_name_param.name,
		       ION_MM_DBG_NAME_LEN - 1);
		client->name[ION_MM_DBG_NAME_LEN - 1] = '\0';
		mutex_unlock(&client->lock);
		return 0;

	default:
		/* GET_CLIENT, SET_HANDLE_BACKTRACE, DMA_OP: as stock, -EFAULT. */
		return -EFAULT;
	}
}

/* ------------------------------------------------------------------------
 * ION_IOC_CUSTOM: ION_CMD_MULTIMEDIA
 */

static long mic_mm_do(struct mic_client *client, struct ion_mm_data *p)
{
	struct mic_buffer *buffer;
	struct dma_buf *dmabuf;
	long ret = 0;

	switch (p->mm_cmd) {
	case ION_MM_CONFIG_BUFFER:
	case ION_MM_CONFIG_BUFFER_EXT:
		if (!p->config_buffer_param.handle)
			return -EFAULT;
		dmabuf = mic_handle_get_dmabuf(client,
					       p->config_buffer_param.handle);
		if (IS_ERR(dmabuf))
			return -EINVAL;
		if (mic_is_ours(dmabuf)) {
			buffer = dmabuf->priv;
			mutex_lock(&buffer->lock);
			buffer->module_id = p->config_buffer_param.module_id;
			buffer->security = p->config_buffer_param.security;
			buffer->coherent = p->config_buffer_param.coherent;
			mutex_unlock(&buffer->lock);
		}
		dma_buf_put(dmabuf);
		return 0;

	case ION_MM_SET_DEBUG_INFO:
		if (!p->buf_debug_info_param.handle)
			return -EFAULT;
		dmabuf = mic_handle_get_dmabuf(client,
					       p->buf_debug_info_param.handle);
		if (IS_ERR(dmabuf))
			return -EINVAL;
		p->buf_debug_info_param.dbg_name[ION_MM_DBG_NAME_LEN - 1] = '\0';
		if (mic_is_ours(dmabuf)) {
			buffer = dmabuf->priv;
			mutex_lock(&buffer->lock);
			memcpy(buffer->dbg_name,
			       p->buf_debug_info_param.dbg_name,
			       ION_MM_DBG_NAME_LEN);
			buffer->dbg_value[0] = p->buf_debug_info_param.value1;
			buffer->dbg_value[1] = p->buf_debug_info_param.value2;
			buffer->dbg_value[2] = p->buf_debug_info_param.value3;
			buffer->dbg_value[3] = p->buf_debug_info_param.value4;
			mutex_unlock(&buffer->lock);
		}
		if (p->buf_debug_info_param.dbg_name[0])
			mic_set_dmabuf_name(dmabuf,
					    p->buf_debug_info_param.dbg_name);
		dma_buf_put(dmabuf);
		return 0;

	case ION_MM_GET_DEBUG_INFO:
		if (!p->buf_debug_info_param.handle)
			return -EFAULT;
		dmabuf = mic_handle_get_dmabuf(client,
					       p->buf_debug_info_param.handle);
		if (IS_ERR(dmabuf))
			return -EINVAL;
		if (mic_is_ours(dmabuf)) {
			buffer = dmabuf->priv;
			mutex_lock(&buffer->lock);
			memcpy(p->buf_debug_info_param.dbg_name,
			       buffer->dbg_name, ION_MM_DBG_NAME_LEN);
			p->buf_debug_info_param.value1 = buffer->dbg_value[0];
			p->buf_debug_info_param.value2 = buffer->dbg_value[1];
			p->buf_debug_info_param.value3 = buffer->dbg_value[2];
			p->buf_debug_info_param.value4 = buffer->dbg_value[3];
			mutex_unlock(&buffer->lock);
		} else {
			ret = -EFAULT;
		}
		dma_buf_put(dmabuf);
		return ret;

	case ION_MM_ACQ_CACHE_POOL:
		return 0;

	case ION_MM_QRY_CACHE_POOL:
		/* No camera page pool is kept. */
		p->pool_info_param.ret = 0;
		return 0;

	case ION_MM_GET_IOVA:
	case ION_MM_GET_IOVA_EXT:
		/*
		 * IOVAs belong to the vendor M4U driver, which does not exist
		 * here; devices get their addresses by attaching the dma-buf.
		 */
		if (!p->get_phys_param.handle)
			return -EFAULT;
		pr_debug("mtk_ion_compat: %s: GET_IOVA is not supported\n",
			 client->name);
		p->get_phys_param.phy_addr = 0;
		p->get_phys_param.len = 0;
		return -EFAULT;

	default:
		/* SET/GET_SF_BUF_INFO and unknown commands: as stock, -EFAULT. */
		return -EFAULT;
	}
}

/* ------------------------------------------------------------------------
 * 32-bit userspace layouts of the custom structures
 */

struct mic_compat_sys_data {
	u32 sys_cmd;
	u32 __pad;
	union {
		struct {
			s32 handle;
			u32 va;
			u32 size;
			u32 sync_type;
			u64 iova;
		} cache_sync_param;
		struct {
			s32 handle;
			u32 phy_addr;
			u32 len;
		} get_phys_param;
		struct {
			char name[ION_MM_DBG_NAME_LEN];
		} client_name_param;
		u8 __size[88];
	};
};

static_assert(sizeof(struct mic_compat_sys_data) == 96);
static_assert(offsetof(struct mic_compat_sys_data, cache_sync_param.iova) == 24);
static_assert(offsetof(struct mic_compat_sys_data, get_phys_param.len) == 16);

struct mic_compat_mm_data {
	u32 mm_cmd;
	u32 __pad;
	union {
		struct {
			s32 handle;
			s32 module_id;
			u32 security;
			u32 coherent;
			u32 reserve_iova_start;
			u32 reserve_iova_end;
		} config_buffer_param;
		struct {
			s32 handle;
			char dbg_name[ION_MM_DBG_NAME_LEN];
			u32 value1;
			u32 value2;
			u32 value3;
			u32 value4;
		} buf_debug_info_param;
		struct {
			u32 len;
			u32 align;
			u32 heap_id_mask;
			u32 flags;
			u32 ret;
		} pool_info_param;
		struct {
			s32 handle;
			s32 module_id;
			u32 security;
			u32 coherent;
			u32 reserve_iova_start;
			u32 reserve_iova_end;
			u64 phy_addr;
			u32 len;
		} get_phys_param;
		u8 __size[72];
	};
};

static_assert(sizeof(struct mic_compat_mm_data) == 80);
static_assert(offsetof(struct mic_compat_mm_data, buf_debug_info_param.value1) == 60);
static_assert(offsetof(struct mic_compat_mm_data, pool_info_param.ret) == 24);
static_assert(offsetof(struct mic_compat_mm_data, get_phys_param.phy_addr) == 32);
static_assert(offsetof(struct mic_compat_mm_data, get_phys_param.len) == 40);

static long mic_sys_ioctl(struct mic_client *client, void __user *uarg,
			  bool compat)
{
	struct mic_compat_sys_data c;
	struct ion_sys_data p;
	long ret;

	if (!uarg)
		return -EINVAL;

	memset(&p, 0, sizeof(p));
	if (!compat) {
		if (copy_from_user(&p, uarg, sizeof(p)))
			return -EFAULT;
	} else {
		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		p.sys_cmd = c.sys_cmd;
		switch (c.sys_cmd) {
		case ION_SYS_CACHE_SYNC:
			p.cache_sync_param.handle = c.cache_sync_param.handle;
			p.cache_sync_param.va = compat_ptr(c.cache_sync_param.va);
			p.cache_sync_param.size = c.cache_sync_param.size;
			p.cache_sync_param.sync_type =
				c.cache_sync_param.sync_type;
			p.cache_sync_param.iova = c.cache_sync_param.iova;
			break;
		case ION_SYS_GET_PHYS:
			p.get_phys_param.handle = c.get_phys_param.handle;
			p.get_phys_param.phy_addr = c.get_phys_param.phy_addr;
			p.get_phys_param.len = c.get_phys_param.len;
			break;
		case ION_SYS_SET_CLIENT_NAME:
			memcpy(p.client_name_param.name,
			       c.client_name_param.name, ION_MM_DBG_NAME_LEN);
			break;
		}
	}

	ret = mic_sys_do(client, &p);
	if (ret)
		return ret;

	if (!compat)
		return copy_to_user(uarg, &p, sizeof(p)) ? -EFAULT : 0;

	if (p.sys_cmd == ION_SYS_GET_PHYS) {
		if (p.get_phys_param.phy_addr > U32_MAX)
			return -EFAULT;
		c.get_phys_param.phy_addr = p.get_phys_param.phy_addr;
		c.get_phys_param.len = p.get_phys_param.len;
	}
	return copy_to_user(uarg, &c, sizeof(c)) ? -EFAULT : 0;
}

static long mic_mm_ioctl(struct mic_client *client, void __user *uarg,
			 bool compat)
{
	struct mic_compat_mm_data c;
	struct ion_mm_data p;
	long ret;

	if (!uarg)
		return -EINVAL;

	memset(&p, 0, sizeof(p));
	if (!compat) {
		if (copy_from_user(&p, uarg, sizeof(p)))
			return -EFAULT;
	} else {
		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		p.mm_cmd = c.mm_cmd;
		switch (c.mm_cmd) {
		case ION_MM_CONFIG_BUFFER:
		case ION_MM_CONFIG_BUFFER_EXT:
			p.config_buffer_param.handle =
				c.config_buffer_param.handle;
			p.config_buffer_param.module_id =
				c.config_buffer_param.module_id;
			p.config_buffer_param.security =
				c.config_buffer_param.security;
			p.config_buffer_param.coherent =
				c.config_buffer_param.coherent;
			p.config_buffer_param.reserve_iova_start =
				c.config_buffer_param.reserve_iova_start;
			p.config_buffer_param.reserve_iova_end =
				c.config_buffer_param.reserve_iova_end;
			break;
		case ION_MM_SET_DEBUG_INFO:
		case ION_MM_GET_DEBUG_INFO:
			p.buf_debug_info_param.handle =
				c.buf_debug_info_param.handle;
			memcpy(p.buf_debug_info_param.dbg_name,
			       c.buf_debug_info_param.dbg_name,
			       ION_MM_DBG_NAME_LEN);
			p.buf_debug_info_param.value1 =
				c.buf_debug_info_param.value1;
			p.buf_debug_info_param.value2 =
				c.buf_debug_info_param.value2;
			p.buf_debug_info_param.value3 =
				c.buf_debug_info_param.value3;
			p.buf_debug_info_param.value4 =
				c.buf_debug_info_param.value4;
			break;
		case ION_MM_ACQ_CACHE_POOL:
		case ION_MM_QRY_CACHE_POOL:
			p.pool_info_param.len = c.pool_info_param.len;
			p.pool_info_param.align = c.pool_info_param.align;
			p.pool_info_param.heap_id_mask =
				c.pool_info_param.heap_id_mask;
			p.pool_info_param.flags = c.pool_info_param.flags;
			p.pool_info_param.ret = c.pool_info_param.ret;
			break;
		case ION_MM_GET_IOVA:
		case ION_MM_GET_IOVA_EXT:
			p.get_phys_param.handle = c.get_phys_param.handle;
			p.get_phys_param.module_id = c.get_phys_param.module_id;
			break;
		}
	}

	ret = mic_mm_do(client, &p);
	if (ret)
		return ret;

	if (!compat)
		return copy_to_user(uarg, &p, sizeof(p)) ? -EFAULT : 0;

	switch (p.mm_cmd) {
	case ION_MM_GET_DEBUG_INFO:
		memcpy(c.buf_debug_info_param.dbg_name,
		       p.buf_debug_info_param.dbg_name, ION_MM_DBG_NAME_LEN);
		c.buf_debug_info_param.value1 = p.buf_debug_info_param.value1;
		c.buf_debug_info_param.value2 = p.buf_debug_info_param.value2;
		c.buf_debug_info_param.value3 = p.buf_debug_info_param.value3;
		c.buf_debug_info_param.value4 = p.buf_debug_info_param.value4;
		break;
	case ION_MM_QRY_CACHE_POOL:
		c.pool_info_param.ret = p.pool_info_param.ret;
		break;
	}
	return copy_to_user(uarg, &c, sizeof(c)) ? -EFAULT : 0;
}

static long mic_custom(struct mic_client *client, unsigned int cmd,
		       void __user *uarg, bool compat)
{
	switch (cmd) {
	case ION_CMD_SYSTEM:
		return mic_sys_ioctl(client, uarg, compat);
	case ION_CMD_MULTIMEDIA:
		return mic_mm_ioctl(client, uarg, compat);
	default:
		/* The stock dispatcher silently accepts unknown classes. */
		return 0;
	}
}

/* ------------------------------------------------------------------------
 * File operations
 */

static long mic_ioctl_common(struct mic_client *client, unsigned int cmd,
			     void __user *uarg)
{
	union {
		struct ion_fd_data fd;
		struct ion_handle_data handle;
		struct ion_heap_query query;
	} data;
	int ret;

	if (copy_from_user(&data, uarg, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case ION_IOC_FREE:
		return mic_handle_free(client, data.handle.handle);

	case ION_IOC_SHARE:
	case ION_IOC_MAP:
		ret = mic_ioc_share(client, data.fd.handle);
		if (ret < 0)
			return ret;
		data.fd.fd = ret;
		/* As in stock ION, the fd stays installed if this fails. */
		return copy_to_user(uarg, &data.fd, sizeof(data.fd)) ?
			-EFAULT : 0;

	case ION_IOC_IMPORT:
		ret = mic_ioc_import(client, data.fd.fd, &data.fd.handle);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &data.fd, sizeof(data.fd))) {
			mic_handle_free(client, data.fd.handle);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_SYNC:
		return mic_ioc_sync(data.fd.fd);

	case ION_IOC_HEAP_QUERY:
		ret = mic_ioc_heap_query(&data.query);
		if (ret)
			return ret;
		return copy_to_user(uarg, &data.query, sizeof(data.query)) ?
			-EFAULT : 0;
	}

	return -ENOTTY;
}

static long mic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mic_client *client = filp->private_data;
	void __user *uarg = (void __user *)arg;
	struct ion_allocation_data alloc;
	struct ion_custom_data custom;
	int id, ret;

	switch (cmd) {
	case ION_IOC_ALLOC:
		if (copy_from_user(&alloc, uarg, sizeof(alloc)))
			return -EFAULT;
		ret = mic_ioc_alloc(client, alloc.len, alloc.heap_id_mask,
				    alloc.flags, &id);
		if (ret)
			return ret;
		alloc.handle = id;
		if (copy_to_user(uarg, &alloc, sizeof(alloc))) {
			mic_handle_free(client, id);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_CUSTOM:
		if (copy_from_user(&custom, uarg, sizeof(custom)))
			return -EFAULT;
		return mic_custom(client, custom.cmd,
				  (void __user *)custom.arg, false);

	case ION_IOC_FREE:
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
	case ION_IOC_IMPORT:
	case ION_IOC_SYNC:
	case ION_IOC_HEAP_QUERY:
		return mic_ioctl_common(client, cmd, uarg);
	}

	return -ENOTTY;
}

#ifdef CONFIG_COMPAT
struct mic_compat_allocation_data {
	compat_size_t len;
	compat_size_t align;
	compat_uint_t heap_id_mask;
	compat_uint_t flags;
	compat_int_t handle;
};

struct mic_compat_custom_data {
	compat_uint_t cmd;
	compat_ulong_t arg;
};

#define COMPAT_ION_IOC_ALLOC	_IOWR(ION_IOC_MAGIC, 0, \
				      struct mic_compat_allocation_data)
#define COMPAT_ION_IOC_CUSTOM	_IOWR(ION_IOC_MAGIC, 6, \
				      struct mic_compat_custom_data)

static long mic_compat_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct mic_client *client = filp->private_data;
	void __user *uarg = compat_ptr(arg);
	struct mic_compat_allocation_data alloc;
	struct mic_compat_custom_data custom;
	int id, ret;

	switch (cmd) {
	case COMPAT_ION_IOC_ALLOC:
		if (copy_from_user(&alloc, uarg, sizeof(alloc)))
			return -EFAULT;
		ret = mic_ioc_alloc(client, alloc.len, alloc.heap_id_mask,
				    alloc.flags, &id);
		if (ret)
			return ret;
		alloc.handle = id;
		if (copy_to_user(uarg, &alloc, sizeof(alloc))) {
			mic_handle_free(client, id);
			return -EFAULT;
		}
		return 0;

	case COMPAT_ION_IOC_CUSTOM:
		if (copy_from_user(&custom, uarg, sizeof(custom)))
			return -EFAULT;
		return mic_custom(client, custom.cmd, compat_ptr(custom.arg),
				  true);

	case ION_IOC_FREE:
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
	case ION_IOC_IMPORT:
	case ION_IOC_SYNC:
	case ION_IOC_HEAP_QUERY:
		/* Same layout for 32-bit and 64-bit callers. */
		return mic_ioctl_common(client, cmd, uarg);
	}

	return -ENOTTY;
}
#endif

static int mic_open(struct inode *inode, struct file *filp)
{
	struct mic_client *client;

	client = kzalloc_obj(*client);
	if (!client)
		return -ENOMEM;

	mutex_init(&client->lock);
	xa_init_flags(&client->handles, XA_FLAGS_ALLOC1);
	client->pid = task_tgid_nr(current);
	get_task_comm(client->name, current->group_leader);

	filp->private_data = client;
	return 0;
}

static int mic_release_file(struct inode *inode, struct file *filp)
{
	struct mic_client *client = filp->private_data;
	struct mic_handle *handle;
	unsigned long idx;

	xa_for_each(&client->handles, idx, handle) {
		dma_buf_put(handle->dmabuf);
		kfree(handle);
	}
	xa_destroy(&client->handles);
	mutex_destroy(&client->lock);
	kfree(client);
	return 0;
}

static const struct file_operations mic_fops = {
	.owner = THIS_MODULE,
	.open = mic_open,
	.release = mic_release_file,
	.unlocked_ioctl = mic_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mic_compat_ioctl,
#endif
};

static struct miscdevice mic_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &mic_fops,
	.mode = 0664,
};

static int __init mic_init(void)
{
	int ret;

	ret = misc_register(&mic_misc);
	if (ret)
		return ret;

	/* Used only for the initial cache clean of new buffers. */
	ret = dma_coerce_mask_and_coherent(mic_misc.this_device,
					   DMA_BIT_MASK(64));
	if (ret) {
		misc_deregister(&mic_misc);
		return ret;
	}

	return 0;
}

static void __exit mic_exit(void)
{
	misc_deregister(&mic_misc);
}

module_init(mic_init);
module_exit(mic_exit);

MODULE_DESCRIPTION("Legacy MediaTek ION ABI on top of DMA-BUF");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
