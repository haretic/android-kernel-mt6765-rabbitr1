// SPDX-License-Identifier: GPL-2.0
/*
 * Legacy MediaTek video codec device (/dev/Vcodec) for MT6765.
 *
 * MT6765 has a single video codec core. Its H.264 encoder (VENC) and its
 * H.264/HEVC/MPEG-4 decoder (VDEC) take turns on it through the VCODEC_SEL
 * mux in vcodecsys, and both reach memory through the VENC ports of SMI
 * larb1. No coprocessor firmware drives it, unlike the SoCs supported by
 * drivers/media/platform/mediatek/vcodec: the stock userspace libraries
 * (libvcodecdrv with the libh264enc/libh264dec/libHEVCdec codecs) parse the
 * bitstreams, program the engines through mmap()ed register windows and
 * point them at buffers whose M4U addresses they get from /dev/ion.
 *
 * The kernel side only arbitrates the core between codec instances, powers
 * it while it is locked, routes VCODEC_SEL and turns the done interrupts
 * into VCODEC_WAITISR wakeups. This driver keeps the ioctl ABI of the stock
 * 4.19 driver (drivers/misc/mediatek/videocodec/mt6765) for native and
 * 32-bit callers. Its bandwidth, DVFS and CPU loading hints have no backend
 * here and are accepted without effect.
 */

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/cpumask.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "../mtk-ion-compat/mtk_ion_compat.h"
#include "mtk_vcodec_abi.h"

static_assert(sizeof(struct vcodec_hw_lock) == 56);
static_assert(sizeof(struct vcodec_hw_lock32) == 32);
static_assert(sizeof(struct vcodec_isr) == 112);
static_assert(sizeof(struct vcodec_isr32) == 96);
static_assert(sizeof(struct vcodec_frame_info) == 32);
static_assert(sizeof(struct vcodec_frame_info32) == 28);

/* vcodecsys (GCON) registers, as used by the stock driver. */
#define GCON_SW_RST0			0x0c
#define GCON_VDEC_ACTIVE		0x10	/* 1 while VDEC owns the core */
#define GCON_SW_RST1			0x14
#define GCON_VCODEC_SEL			0x20
#define   VCODEC_SEL_VENC		0x1
#define   VCODEC_SEL_VDEC		0x2
#define GCON_VENC_DEBLOCK		0x24

/* VDEC: VLD at the base, MISC 0x5000 above it. */
#define VDEC_VLD_STATUS			(61 * 4)
#define   VLD_STATUS_DRAM_BUSY		BIT(15)
#define   VLD_STATUS_DRAM_DONE		BIT(0)
#define VDEC_VLD_SW_RESET		(66 * 4)
#define VDEC_MISC			0x5000
#define VDEC_MISC_INT			(VDEC_MISC + 41 * 4)
#define   VDEC_INT_DONE			BIT(16)
#define   VDEC_INT_CLR			0x11
#define   VDEC_INT_CLR_PULSE		0x10
#define VDEC_MISC_BREAK			(VDEC_MISC + 64 * 4)
#define VDEC_MISC_BREAK_ACK		(VDEC_MISC + 65 * 4)
#define   VDEC_BREAK_OK			0x11

#define VENC_IRQ_STATUS			0x05c
#define VENC_IRQ_ACK			0x060
#define   VENC_IRQ_SPS			BIT(0)
#define   VENC_IRQ_PPS			BIT(1)
#define   VENC_IRQ_FRM			BIT(2)
#define   VENC_IRQ_DRAM			BIT(3)
#define   VENC_IRQ_PAUSE		BIT(4)
#define   VENC_IRQ_SWITCH		BIT(5)
#define   VENC_IRQ_ALL			GENMASK(5, 0)
#define VENC_SW_HRST_N			0x0a8
#define VENC_SW_PAUSE			0x0ac

/* Stock gives a busy encoder lock 30 one-second waits before failing. */
#define VCODEC_ENC_LOCK_TIMEOUT_MS	30000
#define VCODEC_AUTOSUSPEND_MS		100

/*
 * Stock M4U port ids (module ids) of the larb1 engines the blobs tag their
 * buffers with: VENC_RCPU..VENC_RD_COMV and VENC_CUR_LUMA..VENC_REF_CHROMA.
 * The decoder uses the same ports through VCODEC_SEL.
 */
#define STOCK_PORT_VENC_RCPU		8
#define STOCK_PORT_VENC_RD_COMV		12
#define STOCK_PORT_VENC_CUR_LUMA	15
#define STOCK_PORT_VENC_REF_CHROMA	18

enum vcodec_engine {
	VCODEC_VDEC,
	VCODEC_VENC,
};

struct vcodec_client {
	struct vcodec_dev *vdev;
	unsigned int dec_emi_users;
	unsigned int enc_emi_users;
};

struct vcodec_dev {
	struct device *dev;
	struct miscdevice misc;
	void __iomem *venc;
	void __iomem *vdec;
	struct resource *venc_res;
	struct resource *vdec_res;
	struct resource gcon_res;
	struct regmap *gcon;
	struct clk *venc_clk;
	struct clk *vdec_clk;
	int venc_irq;
	int vdec_irq;

	struct mutex lock;		/* owner*, the EMI user counts */
	wait_queue_head_t lock_wq;
	struct vcodec_client *owner;
	u64 owner_handle;
	u32 owner_type;
	enum vcodec_engine owner_engine;
	unsigned int dec_emi_users;
	unsigned int enc_emi_users;

	spinlock_t irq_lock;		/* the done flags, venc_status */
	wait_queue_head_t irq_wq;
	bool vdec_done;
	bool venc_done;
	u32 venc_status;
};

static int vcodec_engine_of(u32 driver_type, enum vcodec_engine *engine)
{
	switch (driver_type) {
	case VAL_DRIVER_TYPE_MP4_DEC:
	case VAL_DRIVER_TYPE_H264_DEC:
	case VAL_DRIVER_TYPE_MP1_MP2_DEC:
	case VAL_DRIVER_TYPE_HEVC_DEC:
		*engine = VCODEC_VDEC;
		return 0;
	case VAL_DRIVER_TYPE_H264_ENC:
		*engine = VCODEC_VENC;
		return 0;
	case VAL_DRIVER_TYPE_JPEG_ENC:
		/* The JPEG encoder has no driver here to use the core with. */
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------------
 * Hardware
 */

static irqreturn_t vcodec_vdec_irq(int irq, void *data)
{
	struct vcodec_dev *vdev = data;
	u32 val;

	val = readl(vdev->vdec + VDEC_MISC_INT);
	if (!(val & VDEC_INT_DONE))
		return IRQ_NONE;

	writel(val | VDEC_INT_CLR, vdev->vdec + VDEC_MISC_INT);
	val = readl(vdev->vdec + VDEC_MISC_INT);
	writel(val & ~VDEC_INT_CLR_PULSE, vdev->vdec + VDEC_MISC_INT);

	spin_lock(&vdev->irq_lock);
	vdev->vdec_done = true;
	spin_unlock(&vdev->irq_lock);
	wake_up_interruptible(&vdev->irq_wq);

	return IRQ_HANDLED;
}

static irqreturn_t vcodec_venc_irq(int irq, void *data)
{
	struct vcodec_dev *vdev = data;
	u32 status;

	status = readl(vdev->venc + VENC_IRQ_STATUS) & VENC_IRQ_ALL;
	if (!status)
		return IRQ_NONE;

	/* Each pending status bit is acknowledged on its own, as on stock. */
	for (u32 bits = status; bits; bits &= bits - 1)
		writel(bits & -bits, vdev->venc + VENC_IRQ_ACK);

	spin_lock(&vdev->irq_lock);
	vdev->venc_status = status;
	vdev->venc_done = true;
	spin_unlock(&vdev->irq_lock);
	wake_up_interruptible(&vdev->irq_wq);

	return IRQ_HANDLED;
}

static int vcodec_gcon_set(struct vcodec_dev *vdev, unsigned int reg, u32 val)
{
	u32 cur;
	int ret;

	ret = regmap_write(vdev->gcon, reg, val);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(vdev->gcon, reg, cur, cur == val,
					10, 1000);
}

static int vcodec_select(struct vcodec_dev *vdev, enum vcodec_engine engine)
{
	u32 sel = engine == VCODEC_VDEC ? VCODEC_SEL_VDEC : VCODEC_SEL_VENC;
	u32 cur;
	int ret;

	if (engine == VCODEC_VENC) {
		ret = regmap_read(vdev->gcon, GCON_VENC_DEBLOCK, &cur);
		if (!ret && !cur)
			ret = vcodec_gcon_set(vdev, GCON_VENC_DEBLOCK, 1);
		if (ret)
			return ret;
	}

	ret = regmap_read(vdev->gcon, GCON_VCODEC_SEL, &cur);
	if (ret)
		return ret;
	if (cur)
		dev_warn(vdev->dev, "VCODEC_SEL was left at %#x\n", cur);
	ret = vcodec_gcon_set(vdev, GCON_VCODEC_SEL, sel);
	if (ret)
		return ret;

	return regmap_write(vdev->gcon, GCON_VDEC_ACTIVE,
			    engine == VCODEC_VDEC);
}

/* Let the decoder's outstanding DRAM accesses drain before it loses power. */
static void vcodec_vdec_drain(struct vcodec_dev *vdev)
{
	u32 val;

	if (readl_poll_timeout(vdev->vdec + VDEC_VLD_STATUS, val,
			       !(val & VLD_STATUS_DRAM_BUSY) ||
			       (val & VLD_STATUS_DRAM_DONE), 10, 20000))
		dev_warn(vdev->dev, "VDEC DRAM accesses pending at power off: VLD %#x MISC_INT %#x\n",
			 val, readl(vdev->vdec + VDEC_MISC_INT));
}

/* Stop a decode the owner abandoned (stock vdec_break()). */
static void vcodec_vdec_break(struct vcodec_dev *vdev)
{
	u32 val;

	writel(1, vdev->vdec + VDEC_MISC_BREAK);
	if (readl_poll_timeout(vdev->vdec + VDEC_MISC_BREAK_ACK, val,
			       (val & VDEC_BREAK_OK) == VDEC_BREAK_OK,
			       10, 10000))
		dev_warn(vdev->dev, "VDEC did not stop: VLD %#x\n",
			 readl(vdev->vdec + VDEC_VLD_STATUS));

	writel(1, vdev->vdec + VDEC_VLD_SW_RESET);
	writel(0, vdev->vdec + VDEC_VLD_SW_RESET);

	regmap_write(vdev->gcon, GCON_SW_RST1, 1);
	regmap_write(vdev->gcon, GCON_SW_RST0, 1);
}

/* Stop an encode the owner abandoned (stock venc_break()). */
static void vcodec_venc_break(struct vcodec_dev *vdev)
{
	u32 val;
	int ret;

	writel(1, vdev->venc + VENC_SW_PAUSE);
	ret = readl_poll_timeout(vdev->venc + VENC_IRQ_STATUS, val,
				 val & VENC_IRQ_PAUSE, 10, 10000);
	if (ret) {
		writel(0, vdev->venc + VENC_SW_PAUSE);
		writel(0, vdev->venc + VENC_SW_HRST_N);
		readl(vdev->venc + VENC_SW_HRST_N);
	} else {
		writel(0, vdev->venc + VENC_SW_HRST_N);
		readl(vdev->venc + VENC_SW_HRST_N);
		writel(0, vdev->venc + VENC_SW_PAUSE);
	}
	writel(1, vdev->venc + VENC_SW_HRST_N);
	readl(vdev->venc + VENC_SW_HRST_N);
	writel(VENC_IRQ_PAUSE, vdev->venc + VENC_IRQ_ACK);
}

static struct clk *vcodec_engine_clk(struct vcodec_dev *vdev,
				     enum vcodec_engine engine)
{
	return engine == VCODEC_VDEC ? vdev->vdec_clk : vdev->venc_clk;
}

static int vcodec_power_on(struct vcodec_dev *vdev, enum vcodec_engine engine)
{
	int ret;

	ret = pm_runtime_resume_and_get(vdev->dev);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vcodec_engine_clk(vdev, engine));
	if (ret)
		goto err_put;

	ret = vcodec_select(vdev, engine);
	if (ret)
		goto err_clk;

	return 0;

err_clk:
	clk_disable_unprepare(vcodec_engine_clk(vdev, engine));
err_put:
	pm_runtime_put_autosuspend(vdev->dev);
	return ret;
}

static void vcodec_power_off(struct vcodec_dev *vdev, enum vcodec_engine engine)
{
	if (engine == VCODEC_VDEC)
		vcodec_vdec_drain(vdev);

	if (vcodec_gcon_set(vdev, GCON_VCODEC_SEL, 0))
		dev_warn(vdev->dev, "VCODEC_SEL did not return to idle\n");

	clk_disable_unprepare(vcodec_engine_clk(vdev, engine));
	pm_runtime_put_autosuspend(vdev->dev);
}

/* ------------------------------------------------------------------------
 * Core lock
 */

static void vcodec_irq_reset(struct vcodec_dev *vdev)
{
	spin_lock_irq(&vdev->irq_lock);
	vdev->vdec_done = false;
	vdev->venc_done = false;
	vdev->venc_status = 0;
	spin_unlock_irq(&vdev->irq_lock);
}

static int vcodec_irq_of(struct vcodec_dev *vdev, enum vcodec_engine engine)
{
	return engine == VCODEC_VDEC ? vdev->vdec_irq : vdev->venc_irq;
}

/* Called with vdev->lock held and the core free. */
static int vcodec_take(struct vcodec_dev *vdev, struct vcodec_client *client,
		       u64 handle, u32 driver_type, enum vcodec_engine engine)
{
	int ret;

	ret = vcodec_power_on(vdev, engine);
	if (ret)
		return ret;

	/* The core was idle: whatever done event is left is stale. */
	vcodec_irq_reset(vdev);

	vdev->owner = client;
	vdev->owner_handle = handle;
	vdev->owner_type = driver_type;
	vdev->owner_engine = engine;

	enable_irq(vcodec_irq_of(vdev, engine));
	return 0;
}

/* Called with vdev->lock held by the owner, or on its behalf. */
static void vcodec_give(struct vcodec_dev *vdev, bool abandoned)
{
	enum vcodec_engine engine = vdev->owner_engine;

	disable_irq(vcodec_irq_of(vdev, engine));

	if (abandoned) {
		dev_warn(vdev->dev, "codec instance closed holding the core (type %u)\n",
			 vdev->owner_type);
		if (engine == VCODEC_VDEC)
			vcodec_vdec_break(vdev);
		else
			vcodec_venc_break(vdev);
	}

	vcodec_power_off(vdev, engine);

	vdev->owner = NULL;
	vdev->owner_handle = 0;
	vdev->owner_type = 0;
	wake_up_interruptible(&vdev->lock_wq);
}

static bool vcodec_is_owner(struct vcodec_dev *vdev,
			    struct vcodec_client *client, u64 handle)
{
	lockdep_assert_held(&vdev->lock);

	return vdev->owner == client && vdev->owner_handle == handle;
}

static int vcodec_lock_hw(struct vcodec_client *client,
			  const struct vcodec_hw_lock *req)
{
	struct vcodec_dev *vdev = client->vdev;
	enum vcodec_engine engine;
	unsigned long deadline, now;
	long left;
	int ret;

	if (!req->handle)
		return -EINVAL;
	ret = vcodec_engine_of(req->driver_type, &engine);
	if (ret) {
		dev_dbg(vdev->dev, "LOCKHW: no lock for type %u\n",
			req->driver_type);
		return ret;
	}
	/* Secure instances need the TEE half of the stock stack. */
	if (req->secure_inst)
		return -EOPNOTSUPP;

	deadline = jiffies + msecs_to_jiffies(VCODEC_ENC_LOCK_TIMEOUT_MS);

	for (;;) {
		mutex_lock(&vdev->lock);
		if (vcodec_is_owner(vdev, client, req->handle)) {
			mutex_unlock(&vdev->lock);
			return -EDEADLK;
		}
		if (!vdev->owner) {
			ret = vcodec_take(vdev, client, req->handle,
					  req->driver_type, engine);
			mutex_unlock(&vdev->lock);
			return ret;
		}
		mutex_unlock(&vdev->lock);

		/* Encoders may only try; decoders wait as long as it takes. */
		if (engine == VCODEC_VDEC) {
			ret = wait_event_interruptible(vdev->lock_wq,
						       !READ_ONCE(vdev->owner));
			if (ret)
				return ret;
			continue;
		}

		if (!req->timeout_ms)
			return -EBUSY;
		now = jiffies;
		if (time_after_eq(now, deadline))
			return -ETIMEDOUT;
		left = wait_event_interruptible_timeout(vdev->lock_wq,
							!READ_ONCE(vdev->owner),
							deadline - now);
		if (left < 0)
			return left;
	}
}

static int vcodec_unlock_hw(struct vcodec_client *client,
			    const struct vcodec_hw_lock *req)
{
	struct vcodec_dev *vdev = client->vdev;
	int ret = 0;

	mutex_lock(&vdev->lock);
	if (vcodec_is_owner(vdev, client, req->handle))
		vcodec_give(vdev, false);
	else
		ret = -EPERM;
	mutex_unlock(&vdev->lock);

	return ret;
}

static int vcodec_wait_isr(struct vcodec_client *client, u64 handle,
			   u32 driver_type, u32 timeout_ms, u32 *status)
{
	struct vcodec_dev *vdev = client->vdev;
	enum vcodec_engine engine;
	bool *done;
	long left;
	int ret;

	ret = vcodec_engine_of(driver_type, &engine);
	if (ret)
		return ret;

	mutex_lock(&vdev->lock);
	if (!vcodec_is_owner(vdev, client, handle)) {
		mutex_unlock(&vdev->lock);
		return -EPERM;
	}
	mutex_unlock(&vdev->lock);

	done = engine == VCODEC_VDEC ? &vdev->vdec_done : &vdev->venc_done;
	left = wait_event_interruptible_timeout(vdev->irq_wq, READ_ONCE(*done),
						msecs_to_jiffies(timeout_ms));
	if (left < 0)
		return left;

	spin_lock_irq(&vdev->irq_lock);
	if (!*done) {
		spin_unlock_irq(&vdev->irq_lock);
		return -ETIMEDOUT;
	}
	*done = false;
	*status = vdev->venc_status;
	spin_unlock_irq(&vdev->irq_lock);

	return 0;
}

/* ------------------------------------------------------------------------
 * ioctl
 */

static int vcodec_get_hw_lock(void __user *uarg, bool compat,
			      struct vcodec_hw_lock *req)
{
	struct vcodec_hw_lock32 req32;

	if (!compat)
		return copy_from_user(req, uarg, sizeof(*req)) ? -EFAULT : 0;

	if (copy_from_user(&req32, uarg, sizeof(req32)))
		return -EFAULT;
	memset(req, 0, sizeof(*req));
	req->handle = req32.handle;
	req->timeout_ms = req32.timeout_ms;
	req->driver_type = req32.driver_type;
	req->secure_inst = req32.secure_inst;
	return 0;
}

static long vcodec_ioctl_wait_isr(struct vcodec_client *client,
				  void __user *uarg, bool compat)
{
	struct vcodec_isr32 isr32;
	struct vcodec_isr isr;
	u32 __user *ustatus;
	u32 status;
	int ret;

	if (!compat) {
		if (copy_from_user(&isr, uarg, sizeof(isr)))
			return -EFAULT;
		ustatus = uarg + offsetof(struct vcodec_isr, irq_status);
	} else {
		if (copy_from_user(&isr32, uarg, sizeof(isr32)))
			return -EFAULT;
		isr.handle = isr32.handle;
		isr.driver_type = isr32.driver_type;
		isr.timeout_ms = isr32.timeout_ms;
		isr.irq_status_num = isr32.irq_status_num;
		ustatus = uarg + offsetof(struct vcodec_isr32, irq_status);
	}

	ret = vcodec_wait_isr(client, isr.handle, isr.driver_type,
			      isr.timeout_ms, &status);
	if (ret)
		return ret;

	/* The encoder reports the interrupt status it was woken by. */
	if (isr.driver_type == VAL_DRIVER_TYPE_H264_ENC && isr.irq_status_num)
		return put_user(status, ustatus);
	return 0;
}

static long vcodec_ioctl_frame_info(void __user *uarg, bool compat)
{
	struct vcodec_frame_info32 info32;
	struct vcodec_frame_info info;

	/* Only read: the EMI bandwidth estimate has no consumer here. */
	if (!compat)
		return copy_from_user(&info, uarg, sizeof(info)) ? -EFAULT : 0;
	return copy_from_user(&info32, uarg, sizeof(info32)) ? -EFAULT : 0;
}

static long vcodec_ioctl_emi_user(struct vcodec_client *client,
				  unsigned int cmd, u32 __user *uarg)
{
	struct vcodec_dev *vdev = client->vdev;
	unsigned int *mine, *all;
	u32 count;

	if (cmd == VCODEC_INC_ENC_EMI_USER || cmd == VCODEC_DEC_ENC_EMI_USER) {
		mine = &client->enc_emi_users;
		all = &vdev->enc_emi_users;
	} else {
		mine = &client->dec_emi_users;
		all = &vdev->dec_emi_users;
	}

	mutex_lock(&vdev->lock);
	if (cmd == VCODEC_INC_ENC_EMI_USER || cmd == VCODEC_INC_DEC_EMI_USER) {
		(*mine)++;
		(*all)++;
	} else if (*mine) {
		(*mine)--;
		(*all)--;
	}
	count = *all;
	mutex_unlock(&vdev->lock);

	return put_user(count, uarg);
}

static long vcodec_ioctl_common(struct file *filp, unsigned int cmd,
				void __user *uarg, bool compat)
{
	struct vcodec_client *client = filp->private_data;
	struct vcodec_hw_lock req;
	int ret;

	switch (cmd) {
	case VCODEC_LOCKHW:
	case VCODEC_UNLOCKHW:
		ret = vcodec_get_hw_lock(uarg, compat, &req);
		if (ret)
			return ret;
		if (cmd == VCODEC_LOCKHW)
			return vcodec_lock_hw(client, &req);
		return vcodec_unlock_hw(client, &req);

	case VCODEC_WAITISR:
		return vcodec_ioctl_wait_isr(client, uarg, compat);

	case VCODEC_SET_FRAME_INFO:
		return vcodec_ioctl_frame_info(uarg, compat);

	case VCODEC_INC_ENC_EMI_USER:
	case VCODEC_DEC_ENC_EMI_USER:
	case VCODEC_INC_DEC_EMI_USER:
	case VCODEC_DEC_DEC_EMI_USER:
		return vcodec_ioctl_emi_user(client, cmd, uarg);

	case VCODEC_GET_CORE_NUMBER:
		return put_user((int)num_possible_cpus(), (int __user *)uarg);

	case VCODEC_MB:
		/* Orders the caller's register writes, as on stock. */
		mb();
		return 0;

	/* Commands the stock driver accepts without doing anything. */
	case MFV_TEST_CMD:
	case MFV_INIT_CMD:
	case MFV_DEINIT_CMD:
	case MFV_SET_CMD_CMD:
	case MFV_SET_PWR_CMD:
	case MFV_SET_ISR_CMD:
	case MFV_ALLOC_MEM_CMD:
	case MFV_FREE_MEM_CMD:
	case MFV_MAKE_PMEM_TO_NONCACHED:
	case MFV_ALLOC_INT_MEM_CMD:
	case MFV_FREE_INT_MEM_CMD:
	case VCODEC_PMEM_FLUSH:
	case VCODEC_PMEM_CLEAN:
	case VCODEC_INC_SYSRAM_USER:
	case VCODEC_DEC_SYSRAM_USER:
	case VCODEC_INITHWLOCK:
	case VCODEC_DEINITHWLOCK:
	case VCODEC_ALLOC_NON_CACHE_BUFFER:
	case VCODEC_FREE_NON_CACHE_BUFFER:
	case VCODEC_SET_THREAD_ID:
	case VCODEC_SET_SYSRAM_INFO:
	case VCODEC_GET_SYSRAM_INFO:
	case VCODEC_INC_PWR_USER:
	case VCODEC_DEC_PWR_USER:
	case VCODEC_GET_CPU_LOADING_INFO:
	case VCODEC_GET_CORE_LOADING:
	case VCODEC_SET_CPU_OPP_LIMIT:
	case VCODEC_SET_LOG_COUNT:
	case VCODEC_SET_AV_TASK_GROUP:
		return 0;
	}

	return -ENOTTY;
}

static long vcodec_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	return vcodec_ioctl_common(filp, cmd, (void __user *)arg, false);
}

#ifdef CONFIG_COMPAT
static long vcodec_compat_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	return vcodec_ioctl_common(filp, cmd, compat_ptr(arg), true);
}
#endif

/* ------------------------------------------------------------------------
 * File operations
 */

/*
 * The blobs map register windows by physical address: the encoder its VENC
 * page, the decoder the VDEC blocks and the first page of vcodecsys.
 */
static int vcodec_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct vcodec_client *client = filp->private_data;
	struct vcodec_dev *vdev = client->vdev;
	phys_addr_t start = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct resource *windows[] = {
		vdev->venc_res, vdev->vdec_res, &vdev->gcon_res,
	};

	for (int i = 0; i < ARRAY_SIZE(windows); i++) {
		struct resource *res = windows[i];

		if (start < res->start || start > res->end ||
		    size > res->end - start + 1)
			continue;

		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
					  size, vma->vm_page_prot);
	}

	dev_dbg(vdev->dev, "refusing to map %pa + %#lx\n", &start, size);
	return -EINVAL;
}

static int vcodec_open(struct inode *inode, struct file *filp)
{
	struct vcodec_dev *vdev = container_of(filp->private_data,
					       struct vcodec_dev, misc);
	struct vcodec_client *client;

	client = kzalloc_obj(*client);
	if (!client)
		return -ENOMEM;

	client->vdev = vdev;
	filp->private_data = client;
	return 0;
}

static int vcodec_release(struct inode *inode, struct file *filp)
{
	struct vcodec_client *client = filp->private_data;
	struct vcodec_dev *vdev = client->vdev;

	mutex_lock(&vdev->lock);
	if (vdev->owner == client)
		vcodec_give(vdev, true);
	vdev->enc_emi_users -= client->enc_emi_users;
	vdev->dec_emi_users -= client->dec_emi_users;
	mutex_unlock(&vdev->lock);

	kfree(client);
	return 0;
}

static const struct file_operations vcodec_fops = {
	.owner = THIS_MODULE,
	.open = vcodec_open,
	.release = vcodec_release,
	.unlocked_ioctl = vcodec_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vcodec_compat_ioctl,
#endif
	.mmap = vcodec_mmap,
};

/* ------------------------------------------------------------------------
 * Platform driver
 */

static int vcodec_get_window(struct platform_device *pdev, const char *name,
			     void __iomem **base, struct resource **res)
{
	*res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	*base = devm_ioremap_resource(&pdev->dev, *res);
	return PTR_ERR_OR_ZERO(*base);
}

static void vcodec_pm_disable(void *data)
{
	struct device *dev = data;

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
}

static int vcodec_get_gcon_window(struct device *dev, struct resource *res)
{
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "mediatek,vcodecsys", 0);
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, res);
	of_node_put(np);
	if (ret)
		return ret;

	/* The decoder only needs the GCON registers at its start. */
	res->end = min(res->end, res->start + PAGE_SIZE - 1);
	return 0;
}

static int vcodec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vcodec_dev *vdev;
	int ret;

	vdev = devm_kzalloc(dev, sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	vdev->dev = dev;
	mutex_init(&vdev->lock);
	init_waitqueue_head(&vdev->lock_wq);
	spin_lock_init(&vdev->irq_lock);
	init_waitqueue_head(&vdev->irq_wq);
	platform_set_drvdata(pdev, vdev);

	ret = vcodec_get_window(pdev, "venc", &vdev->venc, &vdev->venc_res);
	if (ret)
		return ret;
	ret = vcodec_get_window(pdev, "vdec", &vdev->vdec, &vdev->vdec_res);
	if (ret)
		return ret;

	vdev->gcon = syscon_regmap_lookup_by_phandle(dev->of_node,
						     "mediatek,vcodecsys");
	if (IS_ERR(vdev->gcon))
		return dev_err_probe(dev, PTR_ERR(vdev->gcon),
				     "no vcodecsys\n");
	ret = vcodec_get_gcon_window(dev, &vdev->gcon_res);
	if (ret)
		return dev_err_probe(dev, ret, "no vcodecsys window\n");

	vdev->venc_clk = devm_clk_get(dev, "venc");
	if (IS_ERR(vdev->venc_clk))
		return dev_err_probe(dev, PTR_ERR(vdev->venc_clk),
				     "no venc clock\n");
	vdev->vdec_clk = devm_clk_get(dev, "vdec");
	if (IS_ERR(vdev->vdec_clk))
		return dev_err_probe(dev, PTR_ERR(vdev->vdec_clk),
				     "no vdec clock\n");

	vdev->venc_irq = platform_get_irq_byname(pdev, "venc");
	if (vdev->venc_irq < 0)
		return vdev->venc_irq;
	vdev->vdec_irq = platform_get_irq_byname(pdev, "vdec");
	if (vdev->vdec_irq < 0)
		return vdev->vdec_irq;

	/* Unpowered engines may assert their lines: only listen while locked. */
	ret = devm_request_irq(dev, vdev->venc_irq, vcodec_venc_irq,
			       IRQF_NO_AUTOEN, "vcodec-venc", vdev);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, vdev->vdec_irq, vcodec_vdec_irq,
			       IRQF_NO_AUTOEN, "vcodec-vdec", vdev);
	if (ret)
		return ret;

	/* The blobs keep device addresses in 32-bit fields. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = devm_mtk_ion_compat_add_iova_device(dev, STOCK_PORT_VENC_RCPU,
						  STOCK_PORT_VENC_RD_COMV);
	if (ret)
		return ret;
	ret = devm_mtk_ion_compat_add_iova_device(dev, STOCK_PORT_VENC_CUR_LUMA,
						  STOCK_PORT_VENC_REF_CHROMA);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, VCODEC_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev, vcodec_pm_disable, dev);
	if (ret)
		return ret;

	vdev->misc.minor = MISC_DYNAMIC_MINOR;
	vdev->misc.name = "Vcodec";
	vdev->misc.fops = &vcodec_fops;
	vdev->misc.parent = dev;
	return misc_register(&vdev->misc);
}

static void vcodec_remove(struct platform_device *pdev)
{
	struct vcodec_dev *vdev = platform_get_drvdata(pdev);

	/* Open files pin the module and unbinding is suppressed: none is left. */
	misc_deregister(&vdev->misc);
}

static const struct of_device_id vcodec_of_match[] = {
	{ .compatible = "mediatek,mt6765-vcodec" },
	{ }
};
MODULE_DEVICE_TABLE(of, vcodec_of_match);

static struct platform_driver vcodec_driver = {
	.probe = vcodec_probe,
	.remove = vcodec_remove,
	.driver = {
		.name = "mtk-vcodec-compat",
		.of_match_table = vcodec_of_match,
		/* Open files hold pointers into the device data. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(vcodec_driver);

MODULE_DESCRIPTION("Legacy MediaTek MT6765 /dev/Vcodec for the stock video codec blobs");
MODULE_LICENSE("GPL");
