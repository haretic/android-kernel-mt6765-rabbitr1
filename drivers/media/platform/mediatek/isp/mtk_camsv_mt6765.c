// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6765 CAMSV support
 *
 * Unlike the ISP3.0 CAMSV of MT8365, the MT6765 CAMSV has one register window
 * per instance and a frame buffer controller (FBC) that is fed with buffer
 * addresses through FBC_IMGO_ENQ_ADDR, like the MT8183 CAMSV. The register
 * layout below comes from the MT6765 vendor kernel (camera_isp.c), the
 * programming sequence from the MT8183 CAMSV driver of the MediaTek BSP.
 *
 * Only one buffer is handed to the hardware at a time. When no buffer is
 * queued the FBC drops frames until one is.
 */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>

#include "mtk_camsv.h"

/* CAMSV top */
#define MT6765_CAMSV_MODULE_EN			0x0010
#define MT6765_CAMSV_MODULE_EN_TG_EN		BIT(0)
#define MT6765_CAMSV_MODULE_EN_PAK_EN		BIT(2)
#define MT6765_CAMSV_MODULE_EN_PAK_SEL		BIT(3)
#define MT6765_CAMSV_MODULE_EN_IMGO_EN		BIT(4)
#define MT6765_CAMSV_MODULE_EN_DB_EN		BIT(30)
#define MT6765_CAMSV_FMT_SEL			0x0014
#define MT6765_CAMSV_FMT_SEL_RAW8		0x0
#define MT6765_CAMSV_FMT_SEL_RAW10		0x1
#define MT6765_CAMSV_FMT_SEL_RAW12		0x2
#define MT6765_CAMSV_FMT_SEL_YUV422		0x3
#define MT6765_CAMSV_INT_EN			0x0018
#define MT6765_CAMSV_INT_STATUS			0x001c
#define MT6765_CAMSV_SW_CTL			0x0020
#define MT6765_CAMSV_SW_CTL_IMGO_RST_TRIG	BIT(0)
#define MT6765_CAMSV_SW_CTL_IMGO_RST_ST		BIT(1)
#define MT6765_CAMSV_SW_CTL_SW_RST		BIT(2)
#define MT6765_CAMSV_IMGO_FBC			0x002c
#define MT6765_CAMSV_IMGO_FBC_RCNT_INC		BIT(0)
#define MT6765_CAMSV_CLK_EN			0x0030
#define MT6765_CAMSV_CLK_EN_TG_DP		BIT(0)
#define MT6765_CAMSV_CLK_EN_PAK_DP		BIT(2)
#define MT6765_CAMSV_CLK_EN_DMA_DP		BIT(15)
#define MT6765_CAMSV_DATE_CODE			0x003c
#define MT6765_CAMSV_PAK			0x004c
#define MT6765_CAMSV_PAK_RAW8			0x10
#define MT6765_CAMSV_PAK_RAW10			0x11
#define MT6765_CAMSV_PAK_RAW12			0x12

/* Frame buffer controller */
#define MT6765_CAMSV_FBC_IMGO_CTL1		0x0110
#define MT6765_CAMSV_FBC_IMGO_CTL1_FBC_EN	BIT(15)
#define MT6765_CAMSV_FBC_IMGO_CTL1_FBC_MODE	BIT(16)
#define MT6765_CAMSV_FBC_IMGO_CTL1_DMA_RING_EN	BIT(22)
#define MT6765_CAMSV_FBC_IMGO_CTL2		0x0114
#define MT6765_CAMSV_FBC_IMGO_ENQ_ADDR		0x0118
#define MT6765_CAMSV_FBC_IMGO_CUR_ADDR		0x011c

/* IMGO DMA */
#define MT6765_CAMSV_DMA_SOFT_RSTSTAT		0x0200
#define MT6765_CAMSV_IMGO_BASE_ADDR		0x0220
#define MT6765_CAMSV_IMGO_OFST_ADDR		0x0228
#define MT6765_CAMSV_IMGO_XSIZE			0x0230
#define MT6765_CAMSV_IMGO_YSIZE			0x0234
#define MT6765_CAMSV_IMGO_STRIDE		0x0238
#define MT6765_CAMSV_IMGO_STRIDE_RAW		0x01030000
#define MT6765_CAMSV_IMGO_STRIDE_YUV		0x01010000
#define MT6765_CAMSV_IMGO_CON			0x023c
#define MT6765_CAMSV_IMGO_CON2			0x0240
#define MT6765_CAMSV_IMGO_CON3			0x0244
#define MT6765_CAMSV_IMGO_CROP			0x0248
#define MT6765_CAMSV_DMA_ERR_CTRL		0x0440
#define MT6765_CAMSV_IMGO_ERR_STAT		0x0444

/* Timing generator */
#define MT6765_CAMSV_TG_SEN_MODE		0x0500
#define MT6765_CAMSV_TG_SEN_MODE_CMOS_EN	BIT(0)
#define MT6765_CAMSV_TG_SEN_MODE_DBL_DATA_BUS	BIT(1)
#define MT6765_CAMSV_TG_SEN_MODE_TIME_STP_EN	BIT(16)
#define MT6765_CAMSV_TG_VF_CON			0x0504
#define MT6765_CAMSV_TG_VF_CON_VFDATA_EN	BIT(0)
#define MT6765_CAMSV_TG_SEN_GRAB_PXL		0x0508
#define MT6765_CAMSV_TG_SEN_GRAB_LIN		0x050c
#define MT6765_CAMSV_TG_PATH_CFG		0x0510
#define MT6765_CAMSV_TG_PATH_CFG_YUV_U2S_DIS	BIT(12)
#define MT6765_CAMSV_TG_SOF_CNT			0x0520
#define MT6765_CAMSV_TG_ERR_CTL			0x052c
#define MT6765_CAMSV_TG_FRM_CNT_ST		0x0534
#define MT6765_CAMSV_TG_FRMSIZE_ST		0x0538
#define MT6765_CAMSV_TG_INTER_ST		0x053c

/* Interrupts */
#define MT6765_CAMSV_IRQ_TG_ERR			BIT(4)
#define MT6765_CAMSV_IRQ_TG_GBERR		BIT(5)
#define MT6765_CAMSV_IRQ_DROP			BIT(6)
#define MT6765_CAMSV_IRQ_SOF			BIT(7)
#define MT6765_CAMSV_IRQ_HW_PASS1_DON		BIT(10)
#define MT6765_CAMSV_IRQ_IMGO_ERR		BIT(16)
#define MT6765_CAMSV_IRQ_IMGO_OVERR		BIT(17)
#define MT6765_CAMSV_IRQ_SW_PASS1_DON		BIT(20)

#define MT6765_CAMSV_IRQ_ERR \
	(MT6765_CAMSV_IRQ_TG_ERR | MT6765_CAMSV_IRQ_TG_GBERR | \
	 MT6765_CAMSV_IRQ_IMGO_ERR | MT6765_CAMSV_IRQ_IMGO_OVERR)

#define MT6765_CAMSV_IRQ_MASK \
	(MT6765_CAMSV_IRQ_SOF | MT6765_CAMSV_IRQ_HW_PASS1_DON | \
	 MT6765_CAMSV_IRQ_SW_PASS1_DON | MT6765_CAMSV_IRQ_ERR)

static u32 mt6765_camsv_read(struct mtk_cam_dev *cam_dev, u32 reg)
{
	return readl(cam_dev->regs + reg);
}

static void mt6765_camsv_write(struct mtk_cam_dev *cam_dev, u32 reg, u32 value)
{
	writel(value, cam_dev->regs + reg);
}

static void mt6765_camsv_set(struct mtk_cam_dev *cam_dev, u32 reg, u32 bits)
{
	mt6765_camsv_write(cam_dev, reg, mt6765_camsv_read(cam_dev, reg) | bits);
}

static void mt6765_camsv_clr(struct mtk_cam_dev *cam_dev, u32 reg, u32 bits)
{
	mt6765_camsv_write(cam_dev, reg, mt6765_camsv_read(cam_dev, reg) & ~bits);
}

/* Must be called with buf_list_lock held. */
static void mt6765_camsv_enqueue(struct mtk_cam_dev *cam_dev,
				 struct mtk_cam_dev_buffer *buf)
{
	cam_dev->hw_buf = buf;
	if (!buf)
		return;

	mt6765_camsv_write(cam_dev, MT6765_CAMSV_FBC_IMGO_ENQ_ADDR,
			   lower_32_bits(buf->daddr));
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_FBC,
			   MT6765_CAMSV_IMGO_FBC_RCNT_INC);
}

static void mt6765_camsv_setup(struct mtk_cam_dev *cam_dev,
			       unsigned int count, u32 w, u32 h, u32 bpl,
			       const struct mtk_cam_format_info *fmtinfo)
{
	const struct mtk_cam_conf *conf = cam_dev->conf;
	u32 module_en = MT6765_CAMSV_MODULE_EN_PAK_EN;
	u32 stride = MT6765_CAMSV_IMGO_STRIDE_RAW;
	unsigned long flags;
	u32 fmt_sel, pak;
	u32 tmp;
	int ret;

	switch (fmtinfo->bpp) {
	case 8:
		fmt_sel = MT6765_CAMSV_FMT_SEL_RAW8;
		pak = MT6765_CAMSV_PAK_RAW8;
		break;
	case 10:
		fmt_sel = MT6765_CAMSV_FMT_SEL_RAW10;
		pak = MT6765_CAMSV_PAK_RAW10;
		break;
	case 12:
		fmt_sel = MT6765_CAMSV_FMT_SEL_RAW12;
		pak = MT6765_CAMSV_PAK_RAW12;
		break;
	default:
		fmt_sel = MT6765_CAMSV_FMT_SEL_YUV422;
		pak = 0;
		module_en = MT6765_CAMSV_MODULE_EN_PAK_SEL;
		stride = MT6765_CAMSV_IMGO_STRIDE_YUV;
		break;
	}

	mt6765_camsv_write(cam_dev, MT6765_CAMSV_TG_SEN_MODE,
			   conf->tg_sen_mode);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_TG_SEN_GRAB_PXL,
			   (w * fmtinfo->w_factor) << 16U);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_TG_SEN_GRAB_LIN, h << 16U);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_TG_PATH_CFG,
			   MT6765_CAMSV_TG_PATH_CFG_YUV_U2S_DIS);

	/* Reset the CAMSV and its IMGO DMA */
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_SW_CTL,
			   MT6765_CAMSV_SW_CTL_SW_RST);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_SW_CTL, 0);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_SW_CTL,
			   MT6765_CAMSV_SW_CTL_IMGO_RST_TRIG);
	ret = readl_poll_timeout_atomic(cam_dev->regs + MT6765_CAMSV_SW_CTL,
					tmp, tmp == (MT6765_CAMSV_SW_CTL_IMGO_RST_TRIG |
						     MT6765_CAMSV_SW_CTL_IMGO_RST_ST),
					10, 1000);
	if (ret)
		dev_warn(cam_dev->dev, "IMGO reset timeout, SW_CTL 0x%08x\n",
			 tmp);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_SW_CTL, 0);

	/* Reading the status clears it */
	mt6765_camsv_read(cam_dev, MT6765_CAMSV_INT_STATUS);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_INT_EN, MT6765_CAMSV_IRQ_MASK);

	mt6765_camsv_write(cam_dev, MT6765_CAMSV_MODULE_EN,
			   conf->module_en | module_en);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_FMT_SEL, fmt_sel);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_PAK, pak);

	/* FBC in buffer address enqueue mode */
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_FBC_IMGO_CTL1, 0);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_FBC_IMGO_CTL1,
			   MT6765_CAMSV_FBC_IMGO_CTL1_FBC_MODE);

	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_OFST_ADDR, 0);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_CROP, 0);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_XSIZE, bpl - 1U);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_YSIZE, h - 1U);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_STRIDE, stride | bpl);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_CON, conf->imgo_con);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_CON2, conf->imgo_con2);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_IMGO_CON3, conf->imgo_con3);

	/* CMOS_EN first */
	mt6765_camsv_set(cam_dev, MT6765_CAMSV_TG_SEN_MODE,
			 MT6765_CAMSV_TG_SEN_MODE_CMOS_EN);

	/* then FBC_EN and DMA_RING_EN */
	mt6765_camsv_set(cam_dev, MT6765_CAMSV_FBC_IMGO_CTL1,
			 MT6765_CAMSV_FBC_IMGO_CTL1_FBC_EN |
			 MT6765_CAMSV_FBC_IMGO_CTL1_DMA_RING_EN);

	/* finally IMGO_EN */
	mt6765_camsv_set(cam_dev, MT6765_CAMSV_MODULE_EN,
			 MT6765_CAMSV_MODULE_EN_IMGO_EN);

	/* Hand the first queued buffer to the hardware. */
	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	cam_dev->hw_active = true;
	mt6765_camsv_enqueue(cam_dev,
			     list_first_entry_or_null(&cam_dev->buf_list,
						      struct mtk_cam_dev_buffer,
						      list));
	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);
}

static void mt6765_camsv_buf_queue(struct mtk_cam_dev *cam_dev,
				   struct mtk_cam_dev_buffer *buf,
				   bool streaming)
{
	unsigned long flags;

	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	list_add_tail(&buf->list, &cam_dev->buf_list);
	if (cam_dev->hw_active && !cam_dev->hw_buf)
		mt6765_camsv_enqueue(cam_dev, buf);
	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);
}

static void mt6765_camsv_stream_on(struct mtk_cam_dev *cam_dev)
{
	const struct mtk_cam_format_info *fmtinfo = cam_dev->vdev.fmtinfo;
	u32 clk_en = MT6765_CAMSV_CLK_EN_TG_DP | MT6765_CAMSV_CLK_EN_DMA_DP;

	if (fmtinfo->bpp <= 12)
		clk_en |= MT6765_CAMSV_CLK_EN_PAK_DP;

	mt6765_camsv_write(cam_dev, MT6765_CAMSV_CLK_EN, clk_en);
	mt6765_camsv_set(cam_dev, MT6765_CAMSV_TG_VF_CON,
			 MT6765_CAMSV_TG_VF_CON_VFDATA_EN);
}

static void mt6765_camsv_stream_off(struct mtk_cam_dev *cam_dev)
{
	unsigned long flags;

	mt6765_camsv_clr(cam_dev, MT6765_CAMSV_TG_VF_CON,
			 MT6765_CAMSV_TG_VF_CON_VFDATA_EN);
	mt6765_camsv_clr(cam_dev, MT6765_CAMSV_TG_SEN_MODE,
			 MT6765_CAMSV_TG_SEN_MODE_CMOS_EN);
	mt6765_camsv_write(cam_dev, MT6765_CAMSV_INT_EN, 0);
	mt6765_camsv_clr(cam_dev, MT6765_CAMSV_MODULE_EN,
			 MT6765_CAMSV_MODULE_EN_IMGO_EN);
	mt6765_camsv_clr(cam_dev, MT6765_CAMSV_FBC_IMGO_CTL1,
			 MT6765_CAMSV_FBC_IMGO_CTL1_FBC_EN);

	spin_lock_irqsave(&cam_dev->buf_list_lock, flags);
	cam_dev->hw_active = false;
	cam_dev->hw_buf = NULL;
	spin_unlock_irqrestore(&cam_dev->buf_list_lock, flags);

	synchronize_irq(cam_dev->irq);
}

static void mt6765_camsv_resume(struct mtk_cam_dev *cam_dev)
{
	const struct v4l2_pix_format_mplane *fmt = &cam_dev->vdev.format;

	/* The CAM power domain was off, program everything again. */
	mt6765_camsv_setup(cam_dev, 0, fmt->width, fmt->height,
			   fmt->plane_fmt[0].bytesperline,
			   cam_dev->vdev.fmtinfo);
	mt6765_camsv_stream_on(cam_dev);
}

static irqreturn_t mt6765_camsv_isr(int irq, void *data)
{
	struct mtk_cam_dev *cam_dev = data;
	struct mtk_cam_dev_buffer *buf = NULL;
	u32 status;

	status = mt6765_camsv_read(cam_dev, MT6765_CAMSV_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	cam_dev->stats.irqs++;

	if (status & MT6765_CAMSV_IRQ_ERR) {
		cam_dev->stats.errors++;
		cam_dev->stats.last_err = status;
		dev_err_ratelimited(cam_dev->dev,
				    "irq error 0x%08x, frame size 0x%08x, IMGO err 0x%08x\n",
				    status,
				    mt6765_camsv_read(cam_dev, MT6765_CAMSV_TG_FRMSIZE_ST),
				    mt6765_camsv_read(cam_dev, MT6765_CAMSV_IMGO_ERR_STAT));
	}

	if (status & MT6765_CAMSV_IRQ_HW_PASS1_DON)
		cam_dev->stats.hw_done++;

	/* A frame was written to the buffer owned by the hardware. */
	if (status & MT6765_CAMSV_IRQ_SW_PASS1_DON) {
		spin_lock(&cam_dev->buf_list_lock);
		if (cam_dev->hw_active && cam_dev->hw_buf) {
			buf = cam_dev->hw_buf;
			list_del(&buf->list);
			mt6765_camsv_enqueue(cam_dev,
					     list_first_entry_or_null(&cam_dev->buf_list,
								      struct mtk_cam_dev_buffer,
								      list));
		}
		spin_unlock(&cam_dev->buf_list_lock);

		if (buf) {
			cam_dev->stats.done++;
			buf->v4l2_buf.sequence = cam_dev->sequence;
			buf->v4l2_buf.vb2_buf.timestamp = ktime_get_ns();
			vb2_buffer_done(&buf->v4l2_buf.vb2_buf,
					VB2_BUF_STATE_DONE);
		}
	}

	/* Count SOF last, a frame done is reported before the next SOF. */
	if (status & MT6765_CAMSV_IRQ_SOF) {
		cam_dev->stats.sof++;
		cam_dev->sequence++;
	}

	return IRQ_HANDLED;
}

static void mt6765_camsv_dbg_show(struct seq_file *m,
				  struct mtk_cam_dev *cam_dev)
{
	static const struct {
		const char *name;
		u32 offset;
	} regs[] = {
		{ "MODULE_EN", MT6765_CAMSV_MODULE_EN },
		{ "FMT_SEL", MT6765_CAMSV_FMT_SEL },
		{ "INT_EN", MT6765_CAMSV_INT_EN },
		{ "SW_CTL", MT6765_CAMSV_SW_CTL },
		{ "CLK_EN", MT6765_CAMSV_CLK_EN },
		{ "DATE_CODE", MT6765_CAMSV_DATE_CODE },
		{ "PAK", MT6765_CAMSV_PAK },
		{ "FBC_IMGO_CTL1", MT6765_CAMSV_FBC_IMGO_CTL1 },
		{ "FBC_IMGO_CTL2", MT6765_CAMSV_FBC_IMGO_CTL2 },
		{ "FBC_IMGO_ENQ_ADDR", MT6765_CAMSV_FBC_IMGO_ENQ_ADDR },
		{ "FBC_IMGO_CUR_ADDR", MT6765_CAMSV_FBC_IMGO_CUR_ADDR },
		{ "DMA_SOFT_RSTSTAT", MT6765_CAMSV_DMA_SOFT_RSTSTAT },
		{ "IMGO_BASE_ADDR", MT6765_CAMSV_IMGO_BASE_ADDR },
		{ "IMGO_XSIZE", MT6765_CAMSV_IMGO_XSIZE },
		{ "IMGO_YSIZE", MT6765_CAMSV_IMGO_YSIZE },
		{ "IMGO_STRIDE", MT6765_CAMSV_IMGO_STRIDE },
		{ "IMGO_CON", MT6765_CAMSV_IMGO_CON },
		{ "IMGO_CON2", MT6765_CAMSV_IMGO_CON2 },
		{ "IMGO_CON3", MT6765_CAMSV_IMGO_CON3 },
		{ "DMA_ERR_CTRL", MT6765_CAMSV_DMA_ERR_CTRL },
		{ "IMGO_ERR_STAT", MT6765_CAMSV_IMGO_ERR_STAT },
		{ "TG_SEN_MODE", MT6765_CAMSV_TG_SEN_MODE },
		{ "TG_VF_CON", MT6765_CAMSV_TG_VF_CON },
		{ "TG_SEN_GRAB_PXL", MT6765_CAMSV_TG_SEN_GRAB_PXL },
		{ "TG_SEN_GRAB_LIN", MT6765_CAMSV_TG_SEN_GRAB_LIN },
		{ "TG_PATH_CFG", MT6765_CAMSV_TG_PATH_CFG },
		{ "TG_SOF_CNT", MT6765_CAMSV_TG_SOF_CNT },
		{ "TG_ERR_CTL", MT6765_CAMSV_TG_ERR_CTL },
		{ "TG_FRM_CNT_ST", MT6765_CAMSV_TG_FRM_CNT_ST },
		{ "TG_FRMSIZE_ST", MT6765_CAMSV_TG_FRMSIZE_ST },
		{ "TG_INTER_ST", MT6765_CAMSV_TG_INTER_ST },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(m, "%-20s [0x%04x] 0x%08x\n", regs[i].name,
			   regs[i].offset,
			   mt6765_camsv_read(cam_dev, regs[i].offset));
}

static const struct mtk_camsv_hw_ops mt6765_camsv_ops = {
	.setup = mt6765_camsv_setup,
	.buf_queue = mt6765_camsv_buf_queue,
	.stream_on = mt6765_camsv_stream_on,
	.stream_off = mt6765_camsv_stream_off,
	.resume = mt6765_camsv_resume,
	.isr = mt6765_camsv_isr,
	.dbg_show = mt6765_camsv_dbg_show,
};

static const u32 mt6765_camsv_fmts[] = {
	/* The 1st entry is the default image format */
	V4L2_PIX_FMT_MTISP_SBGGR10,
	V4L2_PIX_FMT_MTISP_SGBRG10,
	V4L2_PIX_FMT_MTISP_SGRBG10,
	V4L2_PIX_FMT_MTISP_SRGGB10,
	V4L2_PIX_FMT_SBGGR8,
	V4L2_PIX_FMT_SGBRG8,
	V4L2_PIX_FMT_SGRBG8,
	V4L2_PIX_FMT_SRGGB8,
};

const struct mtk_cam_conf mtk_camsv_mt6765_conf = {
	.ops = &mt6765_camsv_ops,
	.fmts = mt6765_camsv_fmts,
	.num_fmts = ARRAY_SIZE(mt6765_camsv_fmts),
	/* No IOMMU on the IMGO port: the DMA uses 32-bit physical addresses. */
	.dma_bits = 32,
	.tg_sen_mode = MT6765_CAMSV_TG_SEN_MODE_TIME_STP_EN |
		       MT6765_CAMSV_TG_SEN_MODE_DBL_DATA_BUS,
	.module_en = MT6765_CAMSV_MODULE_EN_DB_EN |
		     MT6765_CAMSV_MODULE_EN_TG_EN,
	/* DMA FIFO depth and burst length, priority thresholds */
	.imgo_con = 0x80000080,
	.imgo_con2 = 0x00020002,
	.imgo_con3 = 0x00020002,
};
