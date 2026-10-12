/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 BayLibre
 * Copyright (c) 2022 MediaTek Inc.
 */

#ifndef __MTK_CAMSV_H__
#define __MTK_CAMSV_H__

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <media/media-entity.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>
#include <soc/mediatek/smi.h>

#define IMG_MAX_WIDTH			5376U
#define IMG_MAX_HEIGHT			4032U
#define IMG_MIN_WIDTH			80U
#define IMG_MIN_HEIGHT			60U
#define MAX_SUPPORTED_NUM_PLANES	1U

#define MTK_CAM_CIO_PAD_SENINF		0U
#define MTK_CAM_CIO_PAD_VIDEO		1U
#define MTK_CAM_CIO_NUM_PADS		2U

struct mtk_cam_format_info {
	u32 code;
	u32 fourcc;
	u8 bpp;
	u32 w_factor;
	u32 module_en_pak;
	u32 fmt_sel;
	u32 pak;
	u32 imgo_stride;
};

const struct mtk_cam_format_info *mtk_cam_format_info_by_code(u32 code);


struct mtk_cam_dev_buffer {
	struct vb2_v4l2_buffer v4l2_buf;
	struct list_head list;
	dma_addr_t daddr;
};

/**
 * struct mtk_cam_vdev_desc - MTK camera device descriptor
 * @num_fmts: the number of supported node formats
 * @fmts: supported format
 * @frmsizes: supported V4L2 frame size number
 */
struct mtk_cam_vdev_desc {
	u8 num_fmts;
	const u32 *fmts;
	const struct v4l2_frmsizeenum *frmsizes;
};

/**
 * struct mtk_cam_video_device - MediaTek video device structure
 * @desc: The node description of video device
 * @vdev_pad: The media pad graph object of video device
 * @vdev: The video device instance
 * @vbq: A videobuf queue of video device
 * @vdev_lock: Serializes vb2 queue and video device operations
 * @format: The V4L2 format of video device
 * @fmtinfo: Information about the current format
 */
struct mtk_cam_video_device {
	const struct mtk_cam_vdev_desc *desc;

	struct media_pad vdev_pad;
	struct video_device vdev;
	struct vb2_queue vbq;

	/* Serializes vb2 queue and video device operations */
	struct mutex vdev_lock;

	struct v4l2_pix_format_mplane format;
	const struct mtk_cam_format_info *fmtinfo;
};

struct mtk_cam_dev;
struct seq_file;

/**
 * struct mtk_camsv_hw_ops - CAMSV hardware generation specific operations
 * @fbc_init: Optional, program the frame buffer controller depth when buffers
 *	      are allocated
 * @prepare: Optional, allocate streaming resources for a bytesperline x height
 *	     frame. Called before @setup when streaming starts
 * @unprepare: Optional, release what @prepare allocated. Called after
 *	       @stream_off when streaming stops
 * @setup: Program the hardware for streaming. Called powered, before
 *	   @stream_on, with the vb2 buffers queued so far on buf_list
 * @buf_queue: Add a buffer to buf_list and hand it to the hardware if needed
 * @stream_on: Enable the CMOS and VF data path
 * @stream_off: Stop the data path. Called powered
 * @resume: Restore the streaming state after a runtime resume while streaming
 * @isr: Interrupt handler, data is the struct mtk_cam_dev
 * @dbg_show: Optional, dump hardware state to debugfs. Called powered
 */
struct mtk_camsv_hw_ops {
	void (*fbc_init)(struct mtk_cam_dev *cam_dev, unsigned int num_buffers);
	int (*prepare)(struct mtk_cam_dev *cam_dev, u32 bpl, u32 height);
	void (*unprepare)(struct mtk_cam_dev *cam_dev);
	void (*setup)(struct mtk_cam_dev *cam_dev, unsigned int count,
		      u32 width, u32 height, u32 bpl,
		      const struct mtk_cam_format_info *fmtinfo);
	void (*buf_queue)(struct mtk_cam_dev *cam_dev,
			  struct mtk_cam_dev_buffer *buf, bool streaming);
	void (*stream_on)(struct mtk_cam_dev *cam_dev);
	void (*stream_off)(struct mtk_cam_dev *cam_dev);
	void (*resume)(struct mtk_cam_dev *cam_dev);
	irq_handler_t isr;
	void (*dbg_show)(struct seq_file *m, struct mtk_cam_dev *cam_dev);
};

/**
 * struct mtk_cam_stats - Interrupt statistics, for debugging
 * @irqs: Number of interrupts handled
 * @sof: Number of start of frame interrupts
 * @hw_done: Number of frames ended by the hardware, dropped or not
 * @done: Number of frames written to a buffer
 * @errors: Number of error interrupts
 * @last_err: Last error interrupt status
 */
struct mtk_cam_stats {
	unsigned int irqs;
	unsigned int sof;
	unsigned int hw_done;
	unsigned int done;
	unsigned int errors;
	u32 last_err;
};

/**
 * struct mtk_cam_dev - MediaTek camera device structure.
 * @dev: Pointer to device.
 * @regs: Base address of CAMSV.
 * @regs_img0: Base address of CAMSV IMG0.
 * @regs_tg: Base address of CAMSV TG.
 * @num_clks: Number of clocks.
 * @clks: The clocks.
 * @irq: Irq fired when buffer is ready.
 * @conf: soc specific driver data.
 * @pipeline: Media pipeline information.
 * @subdev: The V4L2 sub-device instance.
 * @subdev_pads: Media pads of this sub-device.
 * @vdev: The video device node.
 * @seninf: Pointer to the seninf pad.
 * @sequence: Buffer sequence number.
 * @buf_list_lock: Protects the buffer list, @hw_slots, @hw_num_slots and
 *		   @hw_active.
 * @buf_list: List head for the buffer list.
 * @hw_slots: Buffers queued to the hardware, oldest first, NULL for the
 *	      scratch buffer (MT6765).
 * @hw_num_slots: Number of valid entries in @hw_slots (MT6765).
 * @hw_active: The hardware is streaming and may consume buffers (MT6765).
 * @scratch: CPU cookie of the buffer frames are dropped into (MT6765).
 * @scratch_daddr: DMA address of @scratch.
 * @scratch_size: Size of @scratch.
 * @stats: Interrupt statistics.
 * @debugfs: debugfs directory.
 */
struct mtk_cam_dev {
	struct device *dev;
	void __iomem *regs;
	void __iomem *regs_img0;
	void __iomem *regs_tg;

	unsigned int num_clks;
	struct clk_bulk_data *clks;
	unsigned int irq;
	const struct mtk_cam_conf *conf;

	struct media_pipeline pipeline;
	struct v4l2_subdev subdev;
	struct media_pad subdev_pads[MTK_CAM_CIO_NUM_PADS];
	struct mtk_cam_video_device vdev;
	struct media_pad *seninf;
	unsigned int sequence;

	spinlock_t buf_list_lock;
	struct list_head buf_list;
	struct mtk_cam_dev_buffer *hw_slots[2];
	unsigned int hw_num_slots;
	bool hw_active;
	void *scratch;
	dma_addr_t scratch_daddr;
	size_t scratch_size;

	struct mtk_cam_stats stats;
	struct dentry *debugfs;
};

/**
 * struct mtk_cam_conf - MediaTek camera configuration structure
 * @ops: hardware generation specific operations
 * @fmts: pixel formats exposed on the video node, the first is the default
 * @num_fmts: number of entries in @fmts
 * @clk_names: clock names, NULL to use all the clocks of the DT node
 * @num_clks: number of entries in @clk_names
 * @split_regs: the IMGO and TG registers are separate memory resources
 * @needs_iommu: defer probing until the device is behind an IOMMU
 * @dma_bits: DMA addressing limit when not behind an IOMMU, 0 for default
 * @tg_sen_mode: TG sensor mode
 * @module_en: module enable
 * @imgo_con: dma control register
 * @imgo_con2: dma control register 2
 * @imgo_con3: dma control register 3
 */
struct mtk_cam_conf {
	const struct mtk_camsv_hw_ops *ops;
	const u32 *fmts;
	u8 num_fmts;
	const char * const *clk_names;
	u8 num_clks;
	bool split_regs;
	bool needs_iommu;
	u8 dma_bits;
	u32 tg_sen_mode;
	u32 module_en;
	u32 imgo_con;
	u32 imgo_con2;
	u32 imgo_con3;
};

extern const struct mtk_cam_conf mtk_camsv_mt6765_conf;

int mtk_cam_dev_init(struct mtk_cam_dev *cam_dev);
void mtk_cam_dev_cleanup(struct mtk_cam_dev *cam_dev);
int mtk_cam_video_register(struct mtk_cam_dev *cam_dev);
void mtk_cam_video_unregister(struct mtk_cam_video_device *vdev);
void mtk_cam_vb2_return_all_buffers(struct mtk_cam_dev *cam,
				    enum vb2_buffer_state state);


#endif /* __MTK_CAMSV_H__ */
