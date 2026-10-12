// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 BayLibre
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>

#include "mtk_camsv.h"
#include "mtk_camsv_reg.h"


#define INT_ST_MASK_CAMSV (CAMSV_IRQ_PASS1_DON)

#define INT_ST_MASK_CAMSV_ERR \
	(CAMSV_IRQ_TG_ERR | CAMSV_IRQ_TG_GBERR | CAMSV_IRQ_IMGO_ERR)

#define MTK_CAMSV_AUTOSUSPEND_DELAY_MS 100

static const struct mtk_camsv_hw_ops mtk_camsv30_ops;

static const char * const mtk_camsv30_clk_names[] = { "cam", "camtg", "camsv" };

static const u32 mtk_camsv30_fmts[] = {
	/* The 1st entry is the default image format */
	V4L2_PIX_FMT_SBGGR8,
	V4L2_PIX_FMT_SGBRG8,
	V4L2_PIX_FMT_SGRBG8,
	V4L2_PIX_FMT_SRGGB8,
	V4L2_PIX_FMT_UYVY,
	V4L2_PIX_FMT_VYUY,
	V4L2_PIX_FMT_YUYV,
	V4L2_PIX_FMT_YVYU,
};

static const struct mtk_cam_conf camsv_conf = {
	.ops = &mtk_camsv30_ops,
	.fmts = mtk_camsv30_fmts,
	.num_fmts = ARRAY_SIZE(mtk_camsv30_fmts),
	.clk_names = mtk_camsv30_clk_names,
	.num_clks = ARRAY_SIZE(mtk_camsv30_clk_names),
	.split_regs = true,
	.needs_iommu = true,
	.tg_sen_mode = CAMSV_TG_SEN_MODE_DBL_DATA_BUS_EN,
	.module_en = CAMSV_MODULE_EN_DB_EN | CAMSV_MODULE_EN_TG_EN,
	.imgo_con = ((0x8  << CAMSV_IMGO_SV_CON_MAX_BURST_LEN_SHIFT) &
		CAMSV_IMGO_SV_CON_MAX_BURST_LEN_MASK) |
		((0x80 << CAMSV_IMGO_SV_CON_FIFO_SIZE_SHIFT) &
		CAMSV_IMGO_SV_CON_FIFO_SIZE_MASK),
	.imgo_con2 = ((0x2 << CAMSV_IMGO_SV_CON2_FIFO_PREP_PRI_THRL_SHIFT) &
		CAMSV_IMGO_SV_CON2_FIFO_PREP_PRI_THRL_MASK) |
		((0x2 << CAMSV_IMGO_SV_CON2_FIFO_PREP_PRI_THRH_SHIFT) |
		CAMSV_IMGO_SV_CON2_FIFO_PREP_PRI_THRH_MASK),
};

static u32 mtk_camsv_read(struct mtk_cam_dev *priv, u32 reg)
{
	return readl(priv->regs + reg);
}

static void mtk_camsv_write(struct mtk_cam_dev *priv, u32 reg, u32 value)
{
	writel(value, priv->regs + reg);
}

static void mtk_camsv_img0_write(struct mtk_cam_dev *priv, u32 reg, u32 value)
{
	writel(value, priv->regs_img0 + reg);
}

static u32 mtk_camsv_tg_read(struct mtk_cam_dev *priv, u32 reg)
{
	return readl(priv->regs_tg + reg);
}

static void mtk_camsv_tg_write(struct mtk_cam_dev *priv, u32 reg, u32 value)
{
	writel(value, priv->regs_tg + reg);
}

static void mtk_camsv_update_buffers_add(struct mtk_cam_dev *cam_dev,
					 struct mtk_cam_dev_buffer *buf)
{
	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_BASE_ADDR, buf->daddr);
}

static void mtk_camsv_cmos_vf_hw_enable(struct mtk_cam_dev *cam_dev)
{
	unsigned int fbc_val;
	u32 clk_en = CAMSV_TG_DP_CLK_EN | CAMSV_DMA_DP_CLK_EN |
		     CAMSV_PAK_DP_CLK_EN;

	fbc_val = mtk_camsv_read(cam_dev, CAMSV_IMGO_FBC);
	fbc_val |= CAMSV_IMGO_FBC_EN;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);

	mtk_camsv_write(cam_dev,  CAMSV_CLK_EN, clk_en);
	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_MODE,
			     mtk_camsv_tg_read(cam_dev, CAMSV_TG_SEN_MODE) |
			     CAMSV_TG_SEN_MODE_CMOS_EN);
	mtk_camsv_tg_write(cam_dev, CAMSV_TG_VF_CON,
			     mtk_camsv_tg_read(cam_dev, CAMSV_TG_VF_CON) |
			     CAMSV_TG_VF_CON_VFDATA_EN);
}

static void mtk_camsv_cmos_vf_hw_disable(struct mtk_cam_dev *cam_dev)
{
	unsigned int fbc_val;

	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_MODE,
			     mtk_camsv_tg_read(cam_dev, CAMSV_TG_SEN_MODE) &
			     ~CAMSV_TG_SEN_MODE_CMOS_EN);
	mtk_camsv_tg_write(cam_dev, CAMSV_TG_VF_CON,
			     mtk_camsv_tg_read(cam_dev, CAMSV_TG_VF_CON) &
			     ~CAMSV_TG_VF_CON_VFDATA_EN);
	fbc_val = mtk_camsv_read(cam_dev, CAMSV_IMGO_FBC);
	fbc_val &= ~CAMSV_IMGO_FBC_EN;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);
}

/*
 * Camsv module is able to handle underrruns using the FBC
 *
 *     - FBC_FB_NUM: the number of userspace buffers available (fifo depth)
 *     - RCNT_INC: user writes 1 when buffer is queued/unqueued
 *     - FBC_CNT: decreased at SOF indicating that the number of buffers filled
 *       by write-DMA decreases. Increased when write-dma done indicating that
 *       the number of buffers filled by write-DMA increases
 *     - RCNT: increased at the end of enqueue indicating that the software
 *       moves to the next buffer for reading
 *     - WCNT: increased to indicate that write-dma writes to the next buffer
 *     Drop condition: FBC_CNT == FBC_NUM
 */

static void mtk_camsv_fbc_init(struct mtk_cam_dev *cam_dev,
			       unsigned int num_buffers)
{
	unsigned int fbc_val;

	if (pm_runtime_resume_and_get(cam_dev->dev) < 0) {
		dev_err(cam_dev->dev, "failed to get pm_runtime\n");
		return;
	}

	fbc_val = mtk_camsv_read(cam_dev, CAMSV_IMGO_FBC);
	fbc_val &= ~CAMSV_IMGO_FBC_FB_NUM;
	fbc_val |= CAMSV_IMGO_FBC_EN;
	fbc_val |= FIELD_PREP(CAMSV_IMGO_FBC_FB_NUM, num_buffers);
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);

	pm_runtime_mark_last_busy(cam_dev->dev);
	pm_runtime_put_autosuspend(cam_dev->dev);
}

static void mtk_camsv_fbc_inc(struct mtk_cam_dev *cam_dev)
{
	unsigned int fbc_val;

	fbc_val = mtk_camsv_read(cam_dev, CAMSV_IMGO_FBC);
	fbc_val |= CAMSV_IMGO_FBC_RCNT_INC;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);
	fbc_val &= ~CAMSV_IMGO_FBC_RCNT_INC;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);
}

static void mtk_camsv_setup(struct mtk_cam_dev *cam_dev, unsigned int count,
			    u32 w, u32 h, u32 bpl,
			    const struct mtk_cam_format_info *fmtinfo)
{
	const struct mtk_cam_conf *conf = cam_dev->conf;
	struct mtk_cam_dev_buffer *buf;
	unsigned long flags;
	unsigned int i;
	u32 tmp;

	for (i = 0; i < count; i++)
		mtk_camsv_fbc_inc(cam_dev);

	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_MODE, conf->tg_sen_mode);

	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_GRAB_PXL,
			     (w * fmtinfo->w_factor) << 16U);

	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_GRAB_LIN, h << 16U);

	/* YUV_U2S_DIS: disable YUV sensor unsigned to signed */
	mtk_camsv_tg_write(cam_dev, CAMSV_TG_PATH_CFG, 0x1000U);

	/* Reset cam */
	mtk_camsv_write(cam_dev, CAMSV_SW_CTL, CAMSV_SW_RST);
	mtk_camsv_write(cam_dev, CAMSV_SW_CTL, 0x0U);
	mtk_camsv_write(cam_dev, CAMSV_SW_CTL, CAMSV_IMGO_RST_TRIG);

	readl_poll_timeout_atomic(cam_dev->regs + CAMSV_SW_CTL, tmp,
				  (tmp == (CAMSV_IMGO_RST_TRIG |
					   CAMSV_IMGO_RST_ST)), 10, 200);

	mtk_camsv_write(cam_dev, CAMSV_SW_CTL, 0x0U);

	mtk_camsv_write(cam_dev, CAMSV_INT_EN, INT_ST_MASK_CAMSV);

	mtk_camsv_write(cam_dev, CAMSV_MODULE_EN,
			  conf->module_en | fmtinfo->module_en_pak);
	mtk_camsv_write(cam_dev, CAMSV_FMT_SEL, fmtinfo->fmt_sel);
	mtk_camsv_write(cam_dev, CAMSV_PAK, fmtinfo->pak);

	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_XSIZE, bpl - 1U);
	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_YSIZE, h - 1U);

	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_STRIDE,
			       fmtinfo->imgo_stride | bpl);

	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_CON, conf->imgo_con);
	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_CON2, conf->imgo_con2);

	/* Set buf addr */
	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	buf = list_first_entry_or_null(&cam_dev->buf_list,
				       struct mtk_cam_dev_buffer,
				       list);
	if (buf)
		mtk_camsv_update_buffers_add(cam_dev, buf);
	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);

	/* CMOS_EN first */
	mtk_camsv_tg_write(cam_dev, CAMSV_TG_SEN_MODE,
			     mtk_camsv_tg_read(cam_dev, CAMSV_TG_SEN_MODE) |
			     CAMSV_TG_SEN_MODE_CMOS_EN);

	/* finally, CAMSV_MODULE_EN : IMGO_EN */
	mtk_camsv_write(cam_dev, CAMSV_MODULE_EN,
			  mtk_camsv_read(cam_dev, CAMSV_MODULE_EN) |
			  CAMSV_MODULE_EN_IMGO_EN);
}

static irqreturn_t isp_irq_camsv(int irq, void *data)
{
	struct mtk_cam_dev *cam_dev = (struct mtk_cam_dev *)data;
	struct mtk_cam_dev_buffer *processed_buf, *buf;
	unsigned int irq_status;

	irq_status = mtk_camsv_read(cam_dev, CAMSV_INT_STATUS);

	if (irq_status & INT_ST_MASK_CAMSV_ERR)
		dev_err(cam_dev->dev, "irq error 0x%lx\n",
			irq_status & INT_ST_MASK_CAMSV_ERR);

	/* De-queue frame */
	if (irq_status & CAMSV_IRQ_PASS1_DON) {
		spin_lock(&cam_dev->buf_list_lock);
		cam_dev->sequence++;

		processed_buf = list_first_entry_or_null(&cam_dev->buf_list,
							 struct mtk_cam_dev_buffer,
							 list);
		if (processed_buf)
			list_del(&processed_buf->list);

		buf = list_first_entry_or_null(&cam_dev->buf_list,
					       struct mtk_cam_dev_buffer,
					       list);

		spin_unlock(&cam_dev->buf_list_lock);

		if (processed_buf) {
			processed_buf->v4l2_buf.sequence = cam_dev->sequence;
			processed_buf->v4l2_buf.vb2_buf.timestamp =
			ktime_get_ns();
			vb2_buffer_done(&processed_buf->v4l2_buf.vb2_buf,
					VB2_BUF_STATE_DONE);
			mtk_camsv_fbc_inc(cam_dev);
		}
		/*
		 * If there is no user buffer available, hardware will drop
		 * automatically frames until buf_queue is called.
		 */
		if (buf)
			mtk_camsv_update_buffers_add(cam_dev, buf);
	}

	return IRQ_HANDLED;
}

static void mtk_camsv_buf_queue(struct mtk_cam_dev *cam_dev,
				struct mtk_cam_dev_buffer *buf, bool streaming)
{
	unsigned long flags;

	/* Add the buffer into the tracking list */
	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	if (streaming && list_empty(&cam_dev->buf_list))
		mtk_camsv_update_buffers_add(cam_dev, buf);

	list_add_tail(&buf->list, &cam_dev->buf_list);
	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);
	if (streaming)
		mtk_camsv_fbc_inc(cam_dev);
}

static void mtk_camsv_resume(struct mtk_cam_dev *cam_dev)
{
	struct mtk_cam_dev_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	buf = list_first_entry_or_null(&cam_dev->buf_list,
				       struct mtk_cam_dev_buffer,
				       list);
	if (buf)
		mtk_camsv_update_buffers_add(cam_dev, buf);

	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);
}

static const struct mtk_camsv_hw_ops mtk_camsv30_ops = {
	.fbc_init = mtk_camsv_fbc_init,
	.setup = mtk_camsv_setup,
	.buf_queue = mtk_camsv_buf_queue,
	.stream_on = mtk_camsv_cmos_vf_hw_enable,
	.stream_off = mtk_camsv_cmos_vf_hw_disable,
	.resume = mtk_camsv_resume,
	.isr = isp_irq_camsv,
};

/* -----------------------------------------------------------------------------
 * debugfs
 */

static int mtk_camsv_stats_show(struct seq_file *m, void *unused)
{
	struct mtk_cam_dev *cam_dev = m->private;
	const struct mtk_cam_stats *stats = &cam_dev->stats;

	seq_printf(m, "irqs:     %u\n", stats->irqs);
	seq_printf(m, "sof:      %u\n", stats->sof);
	seq_printf(m, "hw_done:  %u\n", stats->hw_done);
	seq_printf(m, "done:     %u\n", stats->done);
	seq_printf(m, "errors:   %u\n", stats->errors);
	seq_printf(m, "last_err: 0x%08x\n", stats->last_err);
	seq_printf(m, "sequence: %u\n", cam_dev->sequence);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_camsv_stats);

static int mtk_camsv_regs_show(struct seq_file *m, void *unused)
{
	struct mtk_cam_dev *cam_dev = m->private;

	/* Never power the block up just to read it. */
	if (pm_runtime_get_if_in_use(cam_dev->dev) <= 0) {
		seq_puts(m, "powered off\n");
		return 0;
	}

	cam_dev->conf->ops->dbg_show(m, cam_dev);

	pm_runtime_put(cam_dev->dev);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_camsv_regs);

static void mtk_camsv_debugfs_init(struct mtk_cam_dev *cam_dev)
{
	cam_dev->debugfs = debugfs_create_dir(dev_name(cam_dev->dev), NULL);
	debugfs_create_file("stats", 0400, cam_dev->debugfs, cam_dev,
			    &mtk_camsv_stats_fops);
	if (cam_dev->conf->ops->dbg_show)
		debugfs_create_file("regs", 0400, cam_dev->debugfs, cam_dev,
				    &mtk_camsv_regs_fops);
}

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static void mtk_camsv_release_mem(void *data)
{
	of_reserved_mem_device_release(data);
}

static void mtk_camsv_unlink_larb(void *data)
{
	struct device_link *link = data;
	struct device *larb_dev = link->supplier;

	device_link_del(link);
	put_device(larb_dev);
}

/*
 * Keep the SMI LARB that the IMGO port goes through powered while the CAMSV
 * is active. The LARB must be on for the DMA writes to reach memory, whether
 * or not the port is translated by the IOMMU.
 */
static int mtk_camsv_link_larb(struct mtk_cam_dev *cam_dev)
{
	struct device *dev = cam_dev->dev;
	struct platform_device *larb_pdev;
	struct device_node *larb_node;
	struct device_link *link;

	larb_node = of_parse_phandle(dev->of_node, "mediatek,larb", 0);
	if (!larb_node)
		return 0;

	larb_pdev = of_find_device_by_node(larb_node);
	of_node_put(larb_node);
	if (!larb_pdev || !larb_pdev->dev.driver) {
		if (larb_pdev)
			put_device(&larb_pdev->dev);
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for the SMI LARB\n");
	}

	link = device_link_add(dev, &larb_pdev->dev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
	if (!link) {
		put_device(&larb_pdev->dev);
		return dev_err_probe(dev, -EINVAL,
				     "failed to link to the SMI LARB\n");
	}

	return devm_add_action_or_reset(dev, mtk_camsv_unlink_larb, link);
}

static int mtk_camsv_probe(struct platform_device *pdev)
{
	const struct mtk_cam_conf *conf;
	struct mtk_cam_dev *cam_dev;
	struct device *dev = &pdev->dev;
	unsigned int i;
	int irq;
	int ret;

	conf = device_get_match_data(dev);
	if (!conf)
		return -ENODEV;

	if (conf->needs_iommu && !device_iommu_mapped(dev))
		return -EPROBE_DEFER;

	cam_dev = devm_kzalloc(dev, sizeof(*cam_dev), GFP_KERNEL);
	if (!cam_dev)
		return -ENOMEM;

	cam_dev->conf = conf;
	cam_dev->dev = dev;
	dev_set_drvdata(dev, cam_dev);

	cam_dev->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cam_dev->regs))
		return dev_err_probe(dev, PTR_ERR(cam_dev->regs),
				     "failed to map register base\n");

	if (conf->split_regs) {
		cam_dev->regs_img0 = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(cam_dev->regs_img0))
			return dev_err_probe(dev, PTR_ERR(cam_dev->regs_img0),
					     "failed to map img0 register base\n");

		cam_dev->regs_tg = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(cam_dev->regs_tg))
			return dev_err_probe(dev, PTR_ERR(cam_dev->regs_tg),
					     "failed to map TG register base\n");
	}

	if (conf->clk_names) {
		cam_dev->num_clks = conf->num_clks;
		cam_dev->clks = devm_kcalloc(dev, cam_dev->num_clks,
					     sizeof(*cam_dev->clks),
					     GFP_KERNEL);
		if (!cam_dev->clks)
			return -ENOMEM;

		for (i = 0; i < cam_dev->num_clks; ++i)
			cam_dev->clks[i].id = conf->clk_names[i];

		ret = devm_clk_bulk_get(dev, cam_dev->num_clks, cam_dev->clks);
	} else {
		ret = devm_clk_bulk_get_all(dev, &cam_dev->clks);
		if (ret > 0) {
			cam_dev->num_clks = ret;
			ret = 0;
		} else if (!ret) {
			ret = -ENOENT;
		}
	}
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks: %i\n",
				     ret);

	/* Without an IOMMU the IMGO DMA uses physical addresses. */
	if (conf->dma_bits && !device_iommu_mapped(dev)) {
		ret = dma_set_mask_and_coherent(dev,
						DMA_BIT_MASK(conf->dma_bits));
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set the DMA mask\n");
	}

	/* Optional dedicated CMA pool for the capture buffers. */
	ret = of_reserved_mem_device_init(dev);
	if (!ret) {
		ret = devm_add_action_or_reset(dev, mtk_camsv_release_mem, dev);
		if (ret)
			return ret;
	} else if (ret != -ENODEV) {
		return dev_err_probe(dev, ret,
				     "failed to set up the memory region\n");
	}

	ret = mtk_camsv_link_larb(cam_dev);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	cam_dev->irq = irq;

	spin_lock_init(&cam_dev->buf_list_lock);

	/*
	 * The interrupt is only enabled while the CAM power domain is on:
	 * the status register can't be read with the block powered off.
	 */
	ret = devm_request_irq(dev, cam_dev->irq, conf->ops->isr,
			       IRQF_NO_AUTOEN, dev_name(dev), cam_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq=%d\n",
				     cam_dev->irq);

	/* initialise runtime power management */
	pm_runtime_set_autosuspend_delay(dev, MTK_CAMSV_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/* Initialize the v4l2 common part */
	ret = mtk_cam_dev_init(cam_dev);
	if (ret)
		return ret;

	mtk_camsv_debugfs_init(cam_dev);

	return 0;
}

static void mtk_camsv_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_cam_dev *cam_dev = dev_get_drvdata(dev);

	debugfs_remove_recursive(cam_dev->debugfs);
	mtk_cam_dev_cleanup(cam_dev);
	pm_runtime_dont_use_autosuspend(cam_dev->dev);
	pm_runtime_disable(cam_dev->dev);

}

#ifdef CONFIG_PM
static int mtk_camsv_runtime_suspend(struct device *dev)
{
	struct mtk_cam_dev *cam_dev = dev_get_drvdata(dev);
	struct vb2_queue *vbq = &cam_dev->vdev.vbq;

	if (vb2_is_streaming(vbq)) {
		v4l2_subdev_disable_streams(&cam_dev->subdev,
					    cam_dev->subdev_pads[MTK_CAM_CIO_PAD_VIDEO].index,
					    BIT(0));
	}

	disable_irq(cam_dev->irq);
	clk_bulk_disable_unprepare(cam_dev->num_clks, cam_dev->clks);

	return 0;
}

static int mtk_camsv_runtime_resume(struct device *dev)
{
	struct mtk_cam_dev *cam_dev = dev_get_drvdata(dev);
	struct vb2_queue *vbq = &cam_dev->vdev.vbq;
	int ret;

	ret = clk_bulk_prepare_enable(cam_dev->num_clks, cam_dev->clks);
	if (ret) {
		dev_err(dev, "failed to enable clock:%d\n", ret);
		return ret;
	}

	enable_irq(cam_dev->irq);

	if (vb2_is_streaming(vbq)) {
		cam_dev->conf->ops->resume(cam_dev);

		/* Stream on the sub-device */
		ret = v4l2_subdev_enable_streams(&cam_dev->subdev,
						 cam_dev->subdev_pads[MTK_CAM_CIO_PAD_VIDEO].index,
						 BIT(0));

		if (ret)
			goto fail_no_stream;
	}

	return 0;

fail_no_stream:
	mtk_cam_vb2_return_all_buffers(cam_dev, VB2_BUF_STATE_QUEUED);
	return ret;
}

static const struct dev_pm_ops mtk_camsv_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(mtk_camsv_runtime_suspend,
			   mtk_camsv_runtime_resume, NULL)
};
#endif

static const struct of_device_id mtk_camsv_of_ids[] = {
	{ .compatible = "mediatek,mt6765-camsv", .data = &mtk_camsv_mt6765_conf },
	{ .compatible = "mediatek,mt8365-camsv", .data = &camsv_conf },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_camsv_of_ids);

static struct platform_driver mtk_camsv_driver = {
	.probe = mtk_camsv_probe,
	.remove = mtk_camsv_remove,
	.driver = {
		.name = "mtk-camsv-isp",
		.of_match_table = mtk_camsv_of_ids,
		.pm = &mtk_camsv_pm_ops,
	}
};

module_platform_driver(mtk_camsv_driver);

MODULE_DESCRIPTION("MediaTek CAMSV ISP3.0 driver");
MODULE_AUTHOR("Florian Sylvestre <fsylvestre@baylibre.com>");
MODULE_LICENSE("GPL");
