/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Legacy MediaTek /dev/Vcodec userspace ABI (MT6765 Linux 4.19 vendor kernel).
 *
 * The commands and layouts below reproduce
 * drivers/misc/mediatek/videocodec/mt6765/videocodec_kernel_driver.h and
 * drivers/misc/mediatek/videocodec/include/val_types_public.h from the stock
 * kernel. Every command is declared _IOW(..., unsigned int), whatever it
 * actually carries, so the numbers are the same for 32-bit and 64-bit
 * callers; the argument layouts are not, and both are spelled out here.
 */

#ifndef _MTK_VCODEC_ABI_H
#define _MTK_VCODEC_ABI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MFV_IOC_MAGIC			'M'

#define MFV_TEST_CMD			_IO(MFV_IOC_MAGIC, 0x00)
#define MFV_INIT_CMD			_IO(MFV_IOC_MAGIC, 0x01)
#define MFV_DEINIT_CMD			_IO(MFV_IOC_MAGIC, 0x02)
#define MFV_SET_CMD_CMD			_IOW(MFV_IOC_MAGIC, 0x03, unsigned int)
#define MFV_SET_PWR_CMD			_IOW(MFV_IOC_MAGIC, 0x04, unsigned int)
#define MFV_SET_ISR_CMD			_IOW(MFV_IOC_MAGIC, 0x05, unsigned int)
#define MFV_ALLOC_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x06, unsigned int)
#define MFV_FREE_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x07, unsigned int)
#define MFV_MAKE_PMEM_TO_NONCACHED	_IOW(MFV_IOC_MAGIC, 0x08, unsigned int)
#define MFV_ALLOC_INT_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x09, unsigned int)
#define MFV_FREE_INT_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x0a, unsigned int)
/* struct vcodec_isr */
#define VCODEC_WAITISR			_IOW(MFV_IOC_MAGIC, 0x0b, unsigned int)
/* struct vcodec_hw_lock */
#define VCODEC_LOCKHW			_IOW(MFV_IOC_MAGIC, 0x0d, unsigned int)
#define VCODEC_PMEM_FLUSH		_IOW(MFV_IOC_MAGIC, 0x10, unsigned int)
#define VCODEC_PMEM_CLEAN		_IOW(MFV_IOC_MAGIC, 0x11, unsigned int)
#define VCODEC_INC_SYSRAM_USER		_IOW(MFV_IOC_MAGIC, 0x13, unsigned int)
#define VCODEC_DEC_SYSRAM_USER		_IOW(MFV_IOC_MAGIC, 0x14, unsigned int)
/* __u32 *: the updated user count is written back */
#define VCODEC_INC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x15, unsigned int)
#define VCODEC_DEC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x16, unsigned int)
#define VCODEC_INC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x17, unsigned int)
#define VCODEC_DEC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x18, unsigned int)
#define VCODEC_INITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x20, unsigned int)
#define VCODEC_DEINITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x21, unsigned int)
#define VCODEC_ALLOC_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x22, unsigned int)
#define VCODEC_FREE_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x23, unsigned int)
#define VCODEC_SET_THREAD_ID		_IOW(MFV_IOC_MAGIC, 0x24, unsigned int)
#define VCODEC_SET_SYSRAM_INFO		_IOW(MFV_IOC_MAGIC, 0x25, unsigned int)
#define VCODEC_GET_SYSRAM_INFO		_IOW(MFV_IOC_MAGIC, 0x26, unsigned int)
#define VCODEC_INC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x27, unsigned int)
#define VCODEC_DEC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x28, unsigned int)
#define VCODEC_GET_CPU_LOADING_INFO	_IOW(MFV_IOC_MAGIC, 0x29, unsigned int)
#define VCODEC_GET_CORE_LOADING		_IOW(MFV_IOC_MAGIC, 0x30, unsigned int)
/* int *: the number of CPUs is written back */
#define VCODEC_GET_CORE_NUMBER		_IOW(MFV_IOC_MAGIC, 0x31, unsigned int)
#define VCODEC_SET_CPU_OPP_LIMIT	_IOW(MFV_IOC_MAGIC, 0x32, unsigned int)
/* struct vcodec_hw_lock */
#define VCODEC_UNLOCKHW			_IOW(MFV_IOC_MAGIC, 0x33, unsigned int)
#define VCODEC_MB			_IOW(MFV_IOC_MAGIC, 0x34, unsigned int)
#define VCODEC_SET_LOG_COUNT		_IOW(MFV_IOC_MAGIC, 0x35, unsigned int)
#define VCODEC_SET_AV_TASK_GROUP	_IOW(MFV_IOC_MAGIC, 0x36, unsigned int)
/* struct vcodec_frame_info */
#define VCODEC_SET_FRAME_INFO		_IOW(MFV_IOC_MAGIC, 0x37, unsigned int)

/* enum VAL_DRIVER_TYPE_T, the types the MT6765 core is locked for. */
#define VAL_DRIVER_TYPE_MP4_DEC		2
#define VAL_DRIVER_TYPE_H264_ENC	5
#define VAL_DRIVER_TYPE_H264_DEC	6
#define VAL_DRIVER_TYPE_MP1_MP2_DEC	9
#define VAL_DRIVER_TYPE_JPEG_ENC	23
#define VAL_DRIVER_TYPE_HEVC_DEC	25

#define VCODEC_IRQ_STATUS_MAX		16

/*
 * struct VAL_HW_LOCK_T. The handle is the address of the caller's codec
 * instance and only serves to tell instances apart.
 */
struct vcodec_hw_lock {
	__u64 handle;
	__u32 handle_size;
	__u64 lock;
	__u32 timeout_ms;
	__u64 reserved;
	__u32 reserved_size;
	__u32 driver_type;
	__u8 secure_inst;
};

struct vcodec_hw_lock32 {
	__u32 handle;
	__u32 handle_size;
	__u32 lock;
	__u32 timeout_ms;
	__u32 reserved;
	__u32 reserved_size;
	__u32 driver_type;
	__u8 secure_inst;
};

/* struct VAL_ISR_T */
struct vcodec_isr {
	__u64 handle;
	__u32 handle_size;
	__u32 driver_type;
	__u64 isr_function;
	__u64 reserved;
	__u32 reserved_size;
	__u32 timeout_ms;
	__u32 irq_status_num;
	__u32 irq_status[VCODEC_IRQ_STATUS_MAX];
};

struct vcodec_isr32 {
	__u32 handle;
	__u32 handle_size;
	__u32 driver_type;
	__u32 isr_function;
	__u32 reserved;
	__u32 reserved_size;
	__u32 timeout_ms;
	__u32 irq_status_num;
	__u32 irq_status[VCODEC_IRQ_STATUS_MAX];
};

/* struct VAL_FRAME_INFO_T */
struct vcodec_frame_info {
	__u64 handle;
	__u32 driver_type;
	__u32 input_size;
	__u32 frame_width;
	__u32 frame_height;
	__u32 frame_type;
	__u32 is_compressed;
};

struct vcodec_frame_info32 {
	__u32 handle;
	__u32 driver_type;
	__u32 input_size;
	__u32 frame_width;
	__u32 frame_height;
	__u32 frame_type;
	__u32 is_compressed;
};

#endif /* _MTK_VCODEC_ABI_H */
