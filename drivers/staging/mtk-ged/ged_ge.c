// SPDX-License-Identifier: GPL-2.0
/*
 * GED "gralloc extra" (GE) objects.
 *
 * libgralloc_extra keeps per-buffer MediaTek metadata in GE objects: an
 * anonymous fd holding GE_ALLOC_STRUCT_NUM small regions of 32-bit words.
 * The fd travels with the gralloc handle, so every process importing the
 * buffer sees the same metadata. Behaviour follows the stock MT6765 GED
 * driver (drivers/gpu/mediatek/ged/src/ged_ge.c).
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "ged_priv.h"

struct ged_ge_entry {
	u64 unique_id;
	struct mutex lock;		/* region_data allocation and copies */
	u32 region_sizes[GE_ALLOC_STRUCT_NUM];
	u32 *region_data[GE_ALLOC_STRUCT_NUM];
};

static atomic64_t ged_ge_serial = ATOMIC64_INIT(0);

static int ged_ge_release(struct inode *inode, struct file *file)
{
	struct ged_ge_entry *entry = file->private_data;
	int i;

	for (i = 0; i < GE_ALLOC_STRUCT_NUM; i++)
		kfree(entry->region_data[i]);
	mutex_destroy(&entry->lock);
	kfree(entry);
	return 0;
}

static const struct file_operations ged_ge_fops = {
	.release = ged_ge_release,
};

static int ged_ge_alloc(const u32 *region_sizes)
{
	struct ged_ge_entry *entry;
	int fd, i;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;

	/* The region count is fixed, whatever userspace claims. */
	for (i = 0; i < GE_ALLOC_STRUCT_NUM; i++) {
		if (!region_sizes[i] || region_sizes[i] > GE_MAX_REGION_SIZE) {
			kfree(entry);
			return -EINVAL;
		}
		entry->region_sizes[i] = region_sizes[i];
	}
	mutex_init(&entry->lock);
	entry->unique_id = atomic64_inc_return(&ged_ge_serial) - 1;

	fd = anon_inode_getfd("gralloc_extra", &ged_ge_fops, entry, O_CLOEXEC);
	if (fd < 0) {
		mutex_destroy(&entry->lock);
		kfree(entry);
	}
	return fd;
}

static struct file *ged_ge_fget(int ge_fd)
{
	struct file *file = fget(ge_fd);

	if (!file)
		return NULL;
	if (file->f_op != &ged_ge_fops) {
		fput(file);
		return NULL;
	}
	return file;
}

static bool ged_ge_valid(struct ged_ge_entry *entry, int region_id,
			 int u32_offset, int u32_size)
{
	if (region_id < 0 || region_id >= GE_ALLOC_STRUCT_NUM)
		return false;
	if (u32_offset < 0 || u32_size <= 0)
		return false;
	if (u32_offset >= GE_MAX_REGION_SIZE / sizeof(u32) ||
	    u32_size > GE_MAX_REGION_SIZE / sizeof(u32))
		return false;
	return (u32_offset + u32_size) * sizeof(u32) <=
	       entry->region_sizes[region_id];
}

static int ged_ge_get(int ge_fd, int region_id, int u32_offset, int u32_size,
		      u32 *output)
{
	struct ged_ge_entry *entry;
	struct file *file;
	int err = 0;

	file = ged_ge_fget(ge_fd);
	if (!file)
		return -EFAULT;
	entry = file->private_data;

	if (!ged_ge_valid(entry, region_id, u32_offset, u32_size)) {
		err = -EFAULT;
		goto out;
	}

	mutex_lock(&entry->lock);
	if (entry->region_data[region_id])
		memcpy(output, entry->region_data[region_id] + u32_offset,
		       u32_size * sizeof(u32));
	else
		memset(output, 0, u32_size * sizeof(u32));
	mutex_unlock(&entry->lock);
out:
	fput(file);
	return err;
}

static int ged_ge_set(int ge_fd, int region_id, int u32_offset, int u32_size,
		      const u32 *input)
{
	struct ged_ge_entry *entry;
	struct file *file;
	int err = 0;

	file = ged_ge_fget(ge_fd);
	if (!file)
		return -EFAULT;
	entry = file->private_data;

	if (!ged_ge_valid(entry, region_id, u32_offset, u32_size)) {
		err = -EFAULT;
		goto out;
	}

	mutex_lock(&entry->lock);
	if (!entry->region_data[region_id]) {
		entry->region_data[region_id] =
			kzalloc(entry->region_sizes[region_id], GFP_KERNEL);
		if (!entry->region_data[region_id]) {
			err = -EFAULT;
			goto out_unlock;
		}
	}
	memcpy(entry->region_data[region_id] + u32_offset, input,
	       u32_size * sizeof(u32));
out_unlock:
	mutex_unlock(&entry->lock);
out:
	fput(file);
	return err;
}

int ged_bridge_ge_alloc(struct GED_BRIDGE_IN_GE_ALLOC *in,
			struct GED_BRIDGE_OUT_GE_ALLOC *out)
{
	int fd = ged_ge_alloc(in->region_sizes);

	out->ge_fd = fd < 0 ? -1 : fd;
	out->eError = fd < 0 ? GED_ERROR_OOM : GED_OK;
	return 0;
}

static bool ged_ge_valid_window(int u32_offset, int u32_size)
{
	return u32_offset >= 0 &&
	       u32_offset < GE_MAX_REGION_SIZE / sizeof(u32) &&
	       u32_size > 0 &&
	       u32_size <= GE_MAX_REGION_SIZE / sizeof(u32);
}

int ged_bridge_ge_get(struct GED_BRIDGE_IN_GE_GET *in,
		      struct GED_BRIDGE_OUT_GE_GET *out, int out_size)
{
	if (!ged_ge_valid_window(in->uint32_offset, in->uint32_size))
		return -EFAULT;
	/* libged sizes the output as header + requested words, exactly. */
	if (out_size != sizeof(*out) + in->uint32_size * sizeof(u32))
		return -EFAULT;

	/* As in the stock driver, eError carries the -errno on failure. */
	out->eError = ged_ge_get(in->ge_fd, in->region_id, in->uint32_offset,
				 in->uint32_size, out->data);
	return 0;
}

int ged_bridge_ge_set(struct GED_BRIDGE_IN_GE_SET *in,
		      struct GED_BRIDGE_OUT_GE_SET *out, int in_size)
{
	if (!ged_ge_valid_window(in->uint32_offset, in->uint32_size))
		return -EFAULT;
	if (in_size != sizeof(*in) + in->uint32_size * sizeof(u32))
		return -EFAULT;

	out->eError = ged_ge_set(in->ge_fd, in->region_id, in->uint32_offset,
				 in->uint32_size, in->data);
	return 0;
}

int ged_bridge_ge_info(struct GED_BRIDGE_IN_GE_INFO *in,
		       struct GED_BRIDGE_OUT_GE_INFO *out)
{
	struct ged_ge_entry *entry;
	struct file *file;

	file = ged_ge_fget(in->ge_fd);
	if (!file)
		return -EFAULT;
	entry = file->private_data;
	out->unique_id = entry->unique_id;
	out->eError = GED_OK;
	fput(file);
	return 0;
}
