// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek GED (GPU extension device) compatibility driver.
 *
 * The stock MediaTek graphics blobs (libged.so and everything built on it:
 * libgralloc_extra, the PowerVR EGL/GLES libraries, hwcomposer, codec2)
 * open /proc/ged and issue "bridge" ioctls. This is a minimal port of the
 * MT6765 vendor driver that keeps the exact bridge ABI. The gralloc_extra
 * (GE) objects are implemented fully; log buffers are kept in small
 * in-kernel rings readable from debugfs; DVFS, KPI and FPS hints have no
 * backend on mainline and answer the way the stock driver does when those
 * features are compiled out.
 */

#include <linux/compat.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/linux/dma-buf.h>

#include "ged_priv.h"

#define GED_PROC_NAME		"ged"
/* Largest bridge payload: a GE_SET header plus one full region. */
#define GED_BRIDGE_MAX_SIZE	(sizeof(struct GED_BRIDGE_IN_GE_SET) + \
				 GE_MAX_REGION_SIZE)

#define GED_LOG_ATTR_TIME	0x100
#define GED_LOG_ATTR_TIME_TPT	0x200
#define GED_LOG_LINES		64
#define GED_LOG_LINE_LEN	192

static_assert(sizeof(struct GED_BRIDGE_PACKAGE) == 40);
static_assert(sizeof(struct GED_BRIDGE_OUT_NOTIFY_VSYNC) == 80);
static_assert(sizeof(struct GED_BRIDGE_IN_DVFS_UM_RETURN) == 16);
static_assert(sizeof(struct GED_BRIDGE_IN_GPU_TIMESTAMP) == 32);
static_assert(sizeof(struct GED_BRIDGE_IN_LOGBUFWRITE) == 328);
static_assert(sizeof(struct GED_BRIDGE_OUT_GE_INFO) == 16);
static_assert(sizeof(struct GED_BRIDGE_IN_DMABUF_SET_NAME) == 52);

/* ------------------------------------------------------------------------
 * Log buffers
 *
 * The stock kernel creates these rings at probe time; userspace can only
 * look them up by name and append to them.
 */

struct ged_log_buf {
	const char *name;
	spinlock_t lock;		/* lines, head, count */
	char (*lines)[GED_LOG_LINE_LEN];
	unsigned int head;
	unsigned int count;
};

static struct ged_log_buf ged_log_bufs[] = {
	{ .name = "GPU_FENCE" },
	{ .name = "HWC_err" },
	{ .name = "HWC" },
	{ .name = "FENCE" },
	{ .name = "fence_trace" },
	{ .name = "gfreq" },
};

static struct ged_log_buf *ged_log_buf_from_handle(u32 handle)
{
	if (!handle || handle > ARRAY_SIZE(ged_log_bufs))
		return NULL;
	return &ged_log_bufs[handle - 1];
}

static int ged_bridge_log_buf_get(struct GED_BRIDGE_IN_LOGBUFGET *in,
				  struct GED_BRIDGE_OUT_LOGBUFGET *out)
{
	int i;

	in->acName[GED_LOG_BUF_NAME_LENGTH - 1] = '\0';
	for (i = 0; i < ARRAY_SIZE(ged_log_bufs); i++) {
		if (!strcmp(ged_log_bufs[i].name, in->acName)) {
			out->hLogBuf = i + 1;
			out->eError = GED_OK;
			return 0;
		}
	}
	out->hLogBuf = 0;
	out->eError = GED_ERROR_FAIL;
	return 0;
}

static int ged_bridge_log_buf_write(struct GED_BRIDGE_IN_LOGBUFWRITE *in,
				    struct GED_BRIDGE_OUT_LOGBUFWRITE *out)
{
	struct ged_log_buf *buf = ged_log_buf_from_handle(in->hLogBuf);
	char (*lines)[GED_LOG_LINE_LEN];
	char prefix[48] = "";
	unsigned long flags;
	u32 nsec;
	u64 sec;

	if (!buf) {
		out->eError = GED_ERROR_INVALID_PARAMS;
		return 0;
	}

	in->acLogBuf[GED_BRIDGE_IN_LOGBUF_SIZE - 1] = '\0';
	if (in->attrs & (GED_LOG_ATTR_TIME | GED_LOG_ATTR_TIME_TPT)) {
		sec = div_u64_rem(local_clock(), NSEC_PER_SEC, &nsec);
		if (in->attrs & GED_LOG_ATTR_TIME_TPT)
			scnprintf(prefix, sizeof(prefix), "[%llu.%06u] %d-%d ",
				  sec, nsec / 1000, task_tgid_nr(current),
				  task_pid_nr(current));
		else
			scnprintf(prefix, sizeof(prefix), "[%llu.%06u] ",
				  sec, nsec / 1000);
	}

	if (!READ_ONCE(buf->lines)) {
		lines = kcalloc(GED_LOG_LINES, GED_LOG_LINE_LEN, GFP_KERNEL);
		if (!lines) {
			out->eError = GED_ERROR_OOM;
			return 0;
		}
		spin_lock_irqsave(&buf->lock, flags);
		if (!buf->lines) {
			buf->lines = lines;
			lines = NULL;
		}
		spin_unlock_irqrestore(&buf->lock, flags);
		kfree(lines);
	}

	spin_lock_irqsave(&buf->lock, flags);
	scnprintf(buf->lines[buf->head], GED_LOG_LINE_LEN, "%s%s", prefix,
		  in->acLogBuf);
	buf->head = (buf->head + 1) % GED_LOG_LINES;
	if (buf->count < GED_LOG_LINES)
		buf->count++;
	spin_unlock_irqrestore(&buf->lock, flags);

	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_log_buf_reset(struct GED_BRIDGE_IN_LOGBUFRESET *in,
				    struct GED_BRIDGE_OUT_LOGBUFRESET *out)
{
	struct ged_log_buf *buf = ged_log_buf_from_handle(in->hLogBuf);
	unsigned long flags;

	if (!buf) {
		out->eError = GED_ERROR_INVALID_PARAMS;
		return 0;
	}

	spin_lock_irqsave(&buf->lock, flags);
	buf->head = 0;
	buf->count = 0;
	spin_unlock_irqrestore(&buf->lock, flags);
	out->eError = GED_OK;
	return 0;
}

static int ged_log_show(struct seq_file *s, void *unused)
{
	char *line = kmalloc(GED_LOG_LINE_LEN, GFP_KERNEL);
	struct ged_log_buf *buf;
	unsigned long flags;
	unsigned int i, n, first;
	int b;

	if (!line)
		return -ENOMEM;

	for (b = 0; b < ARRAY_SIZE(ged_log_bufs); b++) {
		buf = &ged_log_bufs[b];
		seq_printf(s, "--- %s ---\n", buf->name);
		spin_lock_irqsave(&buf->lock, flags);
		n = buf->count;
		first = (buf->head + GED_LOG_LINES - n) % GED_LOG_LINES;
		spin_unlock_irqrestore(&buf->lock, flags);

		for (i = 0; i < n; i++) {
			spin_lock_irqsave(&buf->lock, flags);
			strscpy(line, buf->lines[(first + i) % GED_LOG_LINES],
				GED_LOG_LINE_LEN);
			spin_unlock_irqrestore(&buf->lock, flags);
			seq_printf(s, "%s\n", line);
		}
	}

	kfree(line);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ged_log);

/* ------------------------------------------------------------------------
 * DVFS, KPI and hint calls. The values match the stock driver with
 * MTK_GED_KPI off and GED_ENABLE_FB_DVFS on, which is the MT6765 setup.
 */

static int ged_force_mdp_enable;

static int ged_bridge_boost_gpu_freq(struct GED_BRIDGE_IN_BOOSTGPUFREQ *in,
				     struct GED_BRIDGE_OUT_BOOSTGPUFREQ *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_monitor_3d_fence(struct GED_BRIDGE_IN_MONITOR3DFENCE *in,
				       struct GED_BRIDGE_OUT_MONITOR3DFENCE *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_query_info(struct GED_BRIDGE_IN_QUERY_INFO *in,
				 struct GED_BRIDGE_OUT_QUERY_INFO *out)
{
	out->retrieve = 0;
	return 0;
}

static int ged_bridge_notify_vsync(struct GED_BRIDGE_IN_NOTIFY_VSYNC *in,
				   struct GED_BRIDGE_OUT_NOTIFY_VSYNC *out)
{
	/* Frame-based DVFS: userspace must not run its own DVFS policy. */
	out->eError = GED_ERROR_INTENTIONAL_BLOCK;
	return 0;
}

static int ged_bridge_dvfs_probe(struct GED_BRIDGE_IN_DVFS_PROBE *in,
				 struct GED_BRIDGE_OUT_DVFS_PROBE *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_dvfs_um_return(struct GED_BRIDGE_IN_DVFS_UM_RETURN *in,
				     struct GED_BRIDGE_OUT_DVFS_UM_RETURN *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_event_notify(struct GED_BRIDGE_IN_EVENT_NOTIFY *in,
				   struct GED_BRIDGE_OUT_EVENT_NOTIFY *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_gpu_hint_to_cpu(struct GED_BRIDGE_IN_GPU_HINT_TO_CPU *in,
				      struct GED_BRIDGE_OUT_GPU_HINT_TO_CPU *out)
{
	out->eError = GED_OK;
	out->boost_flag = 0;
	out->boost_value = -1;
	return 0;
}

static int ged_bridge_hint_force_mdp(struct GED_BRIDGE_IN_HINT_FORCE_MDP *in,
				     struct GED_BRIDGE_OUT_HINT_FORCE_MDP *out)
{
	if (in->hint != -1)
		WRITE_ONCE(ged_force_mdp_enable, in->hint);
	else
		out->mdp_flag = READ_ONCE(ged_force_mdp_enable);
	out->eError = GED_OK;
	return 0;
}

static int
ged_bridge_query_dvfs_freq_pred(struct GED_BRIDGE_IN_QUERY_DVFS_FREQ_PRED *in,
				struct GED_BRIDGE_OUT_QUERY_DVFS_FREQ_PRED *out)
{
	out->eError = GED_OK;
	return 0;
}

static int
ged_bridge_query_gpu_dvfs_info(struct GED_BRIDGE_IN_QUERY_GPU_DVFS_INFO *in,
			       struct GED_BRIDGE_OUT_QUERY_GPU_DVFS_INFO *out)
{
	out->eError = GED_OK;
	return 0;
}

static int ged_bridge_gpu_timestamp(struct GED_BRIDGE_IN_GPU_TIMESTAMP *in,
				    struct GED_BRIDGE_OUT_GPU_TIMESTAMP *out)
{
	out->is_ged_kpi_enabled = 0;
	out->eError = GED_OK;
	return 0;
}

static int
ged_bridge_gpu_tuner_status(struct GED_BRIDGE_IN_GPU_TUNER_STATUS *in,
			    struct GED_BRIDGE_OUT_GPU_TUNER_STATUS *out)
{
	out->feature = 0;
	return 0;
}

static int
ged_bridge_dmabuf_set_name(struct GED_BRIDGE_IN_DMABUF_SET_NAME *in,
			   struct GED_BRIDGE_OUT_DMABUF_SET_NAME *out)
{
	struct dma_buf *dmabuf;
	char *name;

	dmabuf = dma_buf_get(in->fd);
	if (IS_ERR(dmabuf)) {
		out->eError = GED_ERROR_INVALID_PARAMS;
		return 0;
	}

	in->name[GED_DMABUF_NAME_LEN - 1] = '\0';
	name = kstrndup(in->name, DMA_BUF_NAME_LEN - 1, GFP_KERNEL);
	if (!name) {
		dma_buf_put(dmabuf);
		out->eError = GED_ERROR_OOM;
		return 0;
	}

	spin_lock(&dmabuf->name_lock);
	kfree(dmabuf->name);
	dmabuf->name = name;
	spin_unlock(&dmabuf->name_lock);

	dma_buf_put(dmabuf);
	out->eError = GED_OK;
	return 0;
}

/* ------------------------------------------------------------------------
 * Bridge dispatch
 */

#define GED_CALL(fn, name)						\
	do {								\
		if (pkg->i32InBufferSize <				\
			(int)sizeof(struct GED_BRIDGE_IN_##name) ||	\
		    pkg->i32OutBufferSize <				\
			(int)sizeof(struct GED_BRIDGE_OUT_##name))	\
			goto out;					\
		ret = fn(in, out);					\
	} while (0)

static long ged_dispatch(struct GED_BRIDGE_PACKAGE *pkg)
{
	unsigned int id = GED_GET_BRIDGE_ID(pkg->ui32FunctionID);
	int in_size = pkg->i32InBufferSize;
	void *in = NULL, *out = NULL;
	long ret = -EFAULT;

	if (pkg->i32InBufferSize <= 0 || pkg->i32OutBufferSize <= 0 ||
	    pkg->i32InBufferSize > GED_BRIDGE_MAX_SIZE ||
	    pkg->i32OutBufferSize > GED_BRIDGE_MAX_SIZE)
		return -EFAULT;

	if (id == GED_BRIDGE_COMMAND_GE_ALLOC) {
		/* The stock driver always reads exactly 18 region sizes. */
		in_size = sizeof(struct GED_BRIDGE_IN_GE_ALLOC) +
			  GE_ALLOC_STRUCT_NUM * sizeof(u32);
		if (pkg->i32InBufferSize < in_size)
			return -EFAULT;
	}

	in = memdup_user(pkg->pvParamIn, in_size);
	if (IS_ERR(in))
		return -EFAULT;

	out = kzalloc(pkg->i32OutBufferSize, GFP_KERNEL);
	if (!out)
		goto out;

	switch (id) {
	case GED_BRIDGE_COMMAND_LOG_BUF_GET:
		GED_CALL(ged_bridge_log_buf_get, LOGBUFGET);
		break;
	case GED_BRIDGE_COMMAND_LOG_BUF_WRITE:
		GED_CALL(ged_bridge_log_buf_write, LOGBUFWRITE);
		break;
	case GED_BRIDGE_COMMAND_LOG_BUF_RESET:
		GED_CALL(ged_bridge_log_buf_reset, LOGBUFRESET);
		break;
	case GED_BRIDGE_COMMAND_BOOST_GPU_FREQ:
		GED_CALL(ged_bridge_boost_gpu_freq, BOOSTGPUFREQ);
		break;
	case GED_BRIDGE_COMMAND_MONITOR_3D_FENCE:
		GED_CALL(ged_bridge_monitor_3d_fence, MONITOR3DFENCE);
		break;
	case GED_BRIDGE_COMMAND_QUERY_INFO:
		GED_CALL(ged_bridge_query_info, QUERY_INFO);
		break;
	case GED_BRIDGE_COMMAND_NOTIFY_VSYNC:
		GED_CALL(ged_bridge_notify_vsync, NOTIFY_VSYNC);
		break;
	case GED_BRIDGE_COMMAND_DVFS_PROBE:
		GED_CALL(ged_bridge_dvfs_probe, DVFS_PROBE);
		break;
	case GED_BRIDGE_COMMAND_DVFS_UM_RETURN:
		GED_CALL(ged_bridge_dvfs_um_return, DVFS_UM_RETURN);
		break;
	case GED_BRIDGE_COMMAND_EVENT_NOTIFY:
		GED_CALL(ged_bridge_event_notify, EVENT_NOTIFY);
		break;
	case GED_BRIDGE_COMMAND_GPU_HINT_TO_CPU:
		GED_CALL(ged_bridge_gpu_hint_to_cpu, GPU_HINT_TO_CPU);
		break;
	case GED_BRIDGE_COMMAND_HINT_FORCE_MDP:
		GED_CALL(ged_bridge_hint_force_mdp, HINT_FORCE_MDP);
		break;
	case GED_BRIDGE_COMMAND_QUERY_DVFS_FREQ_PRED:
		GED_CALL(ged_bridge_query_dvfs_freq_pred, QUERY_DVFS_FREQ_PRED);
		break;
	case GED_BRIDGE_COMMAND_QUERY_GPU_DVFS_INFO:
		GED_CALL(ged_bridge_query_gpu_dvfs_info, QUERY_GPU_DVFS_INFO);
		break;
	case GED_BRIDGE_COMMAND_GE_ALLOC:
		GED_CALL(ged_bridge_ge_alloc, GE_ALLOC);
		break;
	case GED_BRIDGE_COMMAND_GE_GET:
		if (pkg->i32InBufferSize <
			(int)sizeof(struct GED_BRIDGE_IN_GE_GET) ||
		    pkg->i32OutBufferSize <
			(int)sizeof(struct GED_BRIDGE_OUT_GE_GET))
			goto out;
		ret = ged_bridge_ge_get(in, out, pkg->i32OutBufferSize);
		break;
	case GED_BRIDGE_COMMAND_GE_SET:
		if (pkg->i32InBufferSize <
			(int)sizeof(struct GED_BRIDGE_IN_GE_SET) ||
		    pkg->i32OutBufferSize <
			(int)sizeof(struct GED_BRIDGE_OUT_GE_SET))
			goto out;
		ret = ged_bridge_ge_set(in, out, pkg->i32InBufferSize);
		break;
	case GED_BRIDGE_COMMAND_GE_INFO:
		GED_CALL(ged_bridge_ge_info, GE_INFO);
		break;
	case GED_BRIDGE_COMMAND_GPU_TIMESTAMP:
		GED_CALL(ged_bridge_gpu_timestamp, GPU_TIMESTAMP);
		break;
	case GED_BRIDGE_COMMAND_GPU_TUNER_STATUS:
		GED_CALL(ged_bridge_gpu_tuner_status, GPU_TUNER_STATUS);
		break;
	case GED_BRIDGE_COMMAND_DMABUF_SET_NAME:
		GED_CALL(ged_bridge_dmabuf_set_name, DMABUF_SET_NAME);
		break;
	default:
		/*
		 * WAIT_HW_VSYNC, QUERY_TARGET_FPS, VSYNC_WAIT, TARGET_FPS and
		 * CREATE_TIMELINE: the stock MT6765 kernel rejects them too.
		 */
		pr_debug("ged: unsupported bridge id %u\n", id);
		break;
	}

	if (copy_to_user(pkg->pvParamOut, out, pkg->i32OutBufferSize))
		ret = -EFAULT;
out:
	kfree(in);
	kfree(out);
	return ret;
}

static long ged_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct GED_BRIDGE_PACKAGE pkg;

	if (_IOC_TYPE(cmd) != GED_MAGIC)
		return -ENOTTY;
	if (copy_from_user(&pkg, (void __user *)arg, sizeof(pkg)))
		return -EFAULT;

	return ged_dispatch(&pkg);
}

#ifdef CONFIG_COMPAT
struct ged_bridge_package32 {
	u32 ui32FunctionID;
	s32 i32Size;
	compat_uptr_t pvParamIn;
	s32 i32InBufferSize;
	compat_uptr_t pvParamOut;
	s32 i32OutBufferSize;
};

static long ged_compat_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct ged_bridge_package32 pkg32;
	struct GED_BRIDGE_PACKAGE pkg;

	if (_IOC_TYPE(cmd) != GED_MAGIC)
		return -ENOTTY;
	if (copy_from_user(&pkg32, compat_ptr(arg), sizeof(pkg32)))
		return -EFAULT;

	pkg.ui32FunctionID = pkg32.ui32FunctionID;
	pkg.i32Size = sizeof(pkg);
	pkg.pvParamIn = compat_ptr(pkg32.pvParamIn);
	pkg.i32InBufferSize = pkg32.i32InBufferSize;
	pkg.pvParamOut = compat_ptr(pkg32.pvParamOut);
	pkg.i32OutBufferSize = pkg32.i32OutBufferSize;

	return ged_dispatch(&pkg);
}
#endif

static const struct proc_ops ged_proc_ops = {
	.proc_ioctl = ged_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = ged_compat_ioctl,
#endif
	.proc_lseek = noop_llseek,
};

static struct dentry *ged_debugfs_dir;

static int __init ged_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ged_log_bufs); i++)
		spin_lock_init(&ged_log_bufs[i].lock);

	if (!proc_create(GED_PROC_NAME, 0644, NULL, &ged_proc_ops)) {
		pr_err("ged: failed to create /proc/%s\n", GED_PROC_NAME);
		return -ENOMEM;
	}

	ged_debugfs_dir = debugfs_create_dir("ged", NULL);
	debugfs_create_file("logbufs", 0444, ged_debugfs_dir, NULL,
			    &ged_log_fops);
	return 0;
}

static void __exit ged_exit(void)
{
	int i;

	debugfs_remove_recursive(ged_debugfs_dir);
	remove_proc_entry(GED_PROC_NAME, NULL);
	for (i = 0; i < ARRAY_SIZE(ged_log_bufs); i++)
		kfree(ged_log_bufs[i].lines);
}

module_init(ged_init);
module_exit(ged_exit);

MODULE_DESCRIPTION("MediaTek GED bridge compatibility driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
