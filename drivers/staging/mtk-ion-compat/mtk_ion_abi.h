/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Legacy MediaTek ION userspace ABI (Linux 4.19 vendor kernels).
 *
 * The layouts below reproduce drivers/staging/android/uapi/mtk_ion/ion.h
 * and drivers/staging/android/mtk_ion/mtk/ion_drv.h from the stock MT6765
 * kernel. Pointer-sized members are kept as pointers and longs, so this
 * header describes the native ABI of whichever compiler includes it: the
 * 64-bit layout in the arm64 kernel and the 32-bit layout in 32-bit
 * userspace. The kernel driver converts the 32-bit layouts explicitly.
 */

#ifndef _MTK_ION_ABI_H
#define _MTK_ION_ABI_H

#include <linux/ioctl.h>
#include <linux/types.h>

typedef int ion_user_handle_t;

/* Stock heap ids; every id is served from ordinary system pages here. */
enum mtk_ion_heap_id {
	MTK_ION_HEAP_SYSTEM = 0,
	MTK_ION_HEAP_SYSTEM_CONTIG = 1,
	MTK_ION_HEAP_CARVEOUT = 2,
	MTK_ION_HEAP_CHUNK = 3,
	MTK_ION_HEAP_DMA = 4,
	MTK_ION_HEAP_MULTIMEDIA = 10,
	MTK_ION_HEAP_FB = 11,
	MTK_ION_HEAP_MULTIMEDIA_FOR_CAMERA = 12,
	MTK_ION_HEAP_MULTIMEDIA_SEC = 13,
	MTK_ION_HEAP_MULTIMEDIA_PROT = 16,
	MTK_ION_HEAP_MULTIMEDIA_2D_FR = 17,
	MTK_ION_HEAP_MULTIMEDIA_WFD = 18,
	MTK_ION_HEAP_MULTIMEDIA_HAPP = 19,
	MTK_ION_HEAP_MULTIMEDIA_HAPP_EXTRA = 20,
	MTK_ION_HEAP_MULTIMEDIA_SDSP = 21,
	MTK_ION_HEAP_MULTIMEDIA_SDSP_SHARED = 22,
};

#define ION_FLAG_CACHED			1
#define ION_FLAG_CACHED_NEEDS_SYNC	2
#define ION_FLAG_FREE_WITHOUT_DEFER	4

struct ion_allocation_data {
	__kernel_size_t len;
	__kernel_size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	ion_user_handle_t handle;
};

struct ion_fd_data {
	ion_user_handle_t handle;
	int fd;
};

struct ion_handle_data {
	ion_user_handle_t handle;
};

struct ion_custom_data {
	unsigned int cmd;
	unsigned long arg;
};

#define MAX_HEAP_NAME			32

struct ion_heap_data {
	char name[MAX_HEAP_NAME];
	__u32 type;
	__u32 heap_id;
	__u32 reserved0;
	__u32 reserved1;
	__u32 reserved2;
};

struct ion_heap_query {
	__u32 cnt;
	__u32 reserved0;
	__u64 heaps;
	__u32 reserved1;
	__u32 reserved2;
};

#define ION_IOC_MAGIC		'I'
#define ION_IOC_ALLOC		_IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE		_IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_MAP		_IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_IOC_SHARE		_IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ION_IOC_IMPORT		_IOWR(ION_IOC_MAGIC, 5, struct ion_fd_data)
#define ION_IOC_CUSTOM		_IOWR(ION_IOC_MAGIC, 6, struct ion_custom_data)
#define ION_IOC_SYNC		_IOWR(ION_IOC_MAGIC, 7, struct ion_fd_data)
#define ION_IOC_HEAP_QUERY	_IOWR(ION_IOC_MAGIC, 8, struct ion_heap_query)

/* ION_IOC_CUSTOM command classes (struct ion_custom_data.cmd). */
enum ION_CMDS {
	ION_CMD_SYSTEM,
	ION_CMD_MULTIMEDIA,
	ION_CMD_MULTIMEDIA_SEC
};

enum ION_MM_CMDS {
	ION_MM_CONFIG_BUFFER,
	ION_MM_SET_DEBUG_INFO,
	ION_MM_GET_DEBUG_INFO,
	ION_MM_SET_SF_BUF_INFO,
	ION_MM_GET_SF_BUF_INFO,
	ION_MM_CONFIG_BUFFER_EXT,
	ION_MM_ACQ_CACHE_POOL,
	ION_MM_QRY_CACHE_POOL,
	ION_MM_GET_IOVA,
	ION_MM_GET_IOVA_EXT,
};

enum ION_SYS_CMDS {
	ION_SYS_CACHE_SYNC,
	ION_SYS_GET_PHYS,
	ION_SYS_GET_CLIENT,
	ION_SYS_SET_HANDLE_BACKTRACE,
	ION_SYS_SET_CLIENT_NAME,
	ION_SYS_DMA_OP,
};

enum ION_CACHE_SYNC_TYPE {
	ION_CACHE_CLEAN_BY_RANGE,
	ION_CACHE_INVALID_BY_RANGE,
	ION_CACHE_FLUSH_BY_RANGE,
	ION_CACHE_CLEAN_BY_RANGE_USE_PA,
	ION_CACHE_INVALID_BY_RANGE_USE_PA,
	ION_CACHE_FLUSH_BY_RANGE_USE_PA,
	ION_CACHE_CLEAN_ALL,
	ION_CACHE_INVALID_ALL,
	ION_CACHE_FLUSH_ALL
};

#define ION_MM_DBG_NAME_LEN		48
#define ION_MM_SF_BUF_INFO_LEN		16
#define ION_SYS_BACKTRACE_SIZE		10

/*
 * In the stock headers each "handle" is a union of the userspace handle id
 * and a kernel pointer, which is why it occupies a full pointer slot.
 */
struct ion_sys_cache_sync_param {
	union {
		int handle;
		void *kernel_handle;
	};
	void *va;
	unsigned int size;
	unsigned int sync_type;		/* enum ION_CACHE_SYNC_TYPE */
	unsigned long long iova;
};

struct ion_sys_get_phys_param {
	union {
		int handle;
		void *kernel_handle;
	};
	unsigned long phy_addr;
	unsigned long len;
};

struct ion_sys_client_name {
	char name[ION_MM_DBG_NAME_LEN];
};

struct ion_sys_get_client_param {
	unsigned int client;
};

struct ion_sys_record_param {
	int group_id;
	int pid;
	unsigned int action;
	unsigned int address_type;
	unsigned int address;
	unsigned int length;
	unsigned int backtrace[ION_SYS_BACKTRACE_SIZE];
	unsigned int backtrace_num;
	void *handle;
	void *client;
	void *buffer;
	void *file;
	int fd;
};

struct ion_dma_param {
	union {
		int handle;
		void *kernel_handle;
	};
	void *va;
	unsigned int size;
	unsigned int dma_type;
	unsigned int dma_dir;
};

struct ion_sys_data {
	unsigned int sys_cmd;		/* enum ION_SYS_CMDS */
	union {
		struct ion_sys_cache_sync_param cache_sync_param;
		struct ion_sys_get_phys_param get_phys_param;
		struct ion_sys_get_client_param get_client_param;
		struct ion_sys_client_name client_name_param;
		struct ion_sys_record_param record_param;
		struct ion_dma_param dma_param;
	};
};

struct ion_mm_config_buffer_param {
	union {
		int handle;
		void *kernel_handle;
	};
	int module_id;
	unsigned int security;
	unsigned int coherent;
	unsigned int reserve_iova_start;
	unsigned int reserve_iova_end;
};

struct ion_mm_buf_debug_info {
	union {
		int handle;
		void *kernel_handle;
	};
	char dbg_name[ION_MM_DBG_NAME_LEN];
	unsigned int value1;
	unsigned int value2;
	unsigned int value3;
	unsigned int value4;
};

struct ion_mm_pool_info {
	__kernel_size_t len;
	__kernel_size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	unsigned int ret;
};

struct ion_mm_get_iova_param {
	union {
		int handle;
		void *kernel_handle;
	};
	int module_id;
	unsigned int security;
	unsigned int coherent;
	unsigned int reserve_iova_start;
	unsigned int reserve_iova_end;
	unsigned long long phy_addr;
	unsigned long len;
};

struct ion_mm_data {
	unsigned int mm_cmd;		/* enum ION_MM_CMDS */
	union {
		struct ion_mm_config_buffer_param config_buffer_param;
		struct ion_mm_buf_debug_info buf_debug_info_param;
		struct ion_mm_pool_info pool_info_param;
		struct ion_mm_get_iova_param get_phys_param;
	};
};

#endif /* _MTK_ION_ABI_H */
