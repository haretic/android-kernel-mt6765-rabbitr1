// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 BayLibre
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "mtk_camsv.h"
#include "mtk_camsv_hw.h"
#include "mtk_camsv_reg.h"


#define INT_ST_MASK_CAMSV (CAMSV_IRQ_PASS1_DON)

#define INT_ST_MASK_CAMSV_ERR \
	(CAMSV_IRQ_TG_ERR | CAMSV_IRQ_TG_GBERR | CAMSV_IRQ_IMGO_ERR)

#define MTK_CAMSV_AUTOSUSPEND_DELAY_MS 100

static const struct mtk_cam_conf camsv_conf = {
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

void mtk_camsv_update_buffers_add(struct mtk_cam_dev *cam_dev,
					   struct mtk_cam_dev_buffer *buf)
{
	mtk_camsv_img0_write(cam_dev, CAMSV_IMGO_SV_BASE_ADDR, buf->daddr);
}

void mtk_camsv_cmos_vf_hw_enable(struct mtk_cam_dev *cam_dev)
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

void mtk_camsv_cmos_vf_hw_disable(struct mtk_cam_dev *cam_dev)
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

void mtk_camsv_fbc_init(struct mtk_cam_dev *cam_dev,
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

void mtk_camsv_fbc_inc(struct mtk_cam_dev *cam_dev)
{
	unsigned int fbc_val;

	fbc_val = mtk_camsv_read(cam_dev, CAMSV_IMGO_FBC);
	fbc_val |= CAMSV_IMGO_FBC_RCNT_INC;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);
	fbc_val &= ~CAMSV_IMGO_FBC_RCNT_INC;
	mtk_camsv_write(cam_dev, CAMSV_IMGO_FBC, fbc_val);
}

void mtk_camsv_setup(struct mtk_cam_dev *cam_dev, unsigned int count,
		     u32 w, u32 h, u32 bpl, u32 mbus_fmt)
{
	const struct mtk_cam_conf *conf = cam_dev->conf;
	const struct mtk_cam_format_info *fmtinfo;
	struct mtk_cam_dev_buffer *buf;
	unsigned long flags;
	unsigned int i;
	u32 tmp;

	for (i = 0; i < count; i++)
		mtk_camsv_fbc_inc(cam_dev);

	fmtinfo = mtk_cam_format_info_by_code(mbus_fmt);

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

static int mtk_camsv_probe(struct platform_device *pdev)
{
	static const char * const clk_names[] = { "cam", "camtg", "camsv"};

	struct mtk_cam_dev *cam_dev;
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	if (!device_iommu_mapped(dev))
		return -EPROBE_DEFER;

	cam_dev = devm_kzalloc(dev, sizeof(*cam_dev), GFP_KERNEL);
	if (!cam_dev)
		return -ENOMEM;

	cam_dev->conf = device_get_match_data(dev);
	if (!cam_dev->conf)
		return -ENODEV;

	cam_dev->dev = dev;
	dev_set_drvdata(dev, cam_dev);

	cam_dev->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cam_dev->regs))
		return dev_err_probe(dev, PTR_ERR(cam_dev->regs),
				     "failed to map register base\n");

	cam_dev->regs_img0 = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(cam_dev->regs_img0))
		return dev_err_probe(dev, PTR_ERR(cam_dev->regs_img0),
				     "failed to map img0 register base\n");

	cam_dev->regs_tg = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(cam_dev->regs_tg))
		return dev_err_probe(dev, PTR_ERR(cam_dev->regs_tg),
				     "failed to map TG register base\n");

	cam_dev->num_clks = ARRAY_SIZE(clk_names);
	cam_dev->clks = devm_kcalloc(dev, cam_dev->num_clks,
				     sizeof(*cam_dev->clks), GFP_KERNEL);
	if (!cam_dev->clks)
		return -ENOMEM;

	for (i = 0; i < cam_dev->num_clks; ++i)
		cam_dev->clks[i].id = clk_names[i];

	ret = devm_clk_bulk_get(dev, cam_dev->num_clks, cam_dev->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks: %i\n",
				     ret);

	cam_dev->irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, cam_dev->irq, isp_irq_camsv, 0,
			       dev_name(dev), cam_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq=%d\n",
				     cam_dev->irq);

	spin_lock_init(&cam_dev->buf_list_lock);

	/* initialise runtime power management */
	pm_runtime_set_autosuspend_delay(dev, MTK_CAMSV_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	devm_pm_runtime_enable(dev);

	/* Initialize the v4l2 common part */
	return mtk_cam_dev_init(cam_dev);
}

static void mtk_camsv_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_cam_dev *cam_dev = dev_get_drvdata(dev);

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

	clk_bulk_disable_unprepare(cam_dev->num_clks, cam_dev->clks);

	return 0;
}

static int mtk_camsv_runtime_resume(struct device *dev)
{
	struct mtk_cam_dev *cam_dev = dev_get_drvdata(dev);
	struct vb2_queue *vbq = &cam_dev->vdev.vbq;
	struct mtk_cam_dev_buffer *buf;
	unsigned long flags = 0;
	int ret;

	ret = clk_bulk_prepare_enable(cam_dev->num_clks, cam_dev->clks);
	if (ret) {
		dev_err(dev, "failed to enable clock:%d\n", ret);
		return ret;
	}

	if (vb2_is_streaming(vbq)) {

		spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
		buf = list_first_entry_or_null(&cam_dev->buf_list,
					       struct mtk_cam_dev_buffer,
					       list);
		if (buf)
			mtk_camsv_update_buffers_add(cam_dev, buf);

		spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);

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
