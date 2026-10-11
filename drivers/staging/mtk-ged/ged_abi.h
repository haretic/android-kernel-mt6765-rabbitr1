/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * MediaTek GED (/proc/ged) bridge ABI, as used by the stock libged.so.
 *
 * Taken from the MT6765 vendor kernel (drivers/gpu/mediatek/ged/include/
 * ged_bridge_id.h, ged_type.h and src/ged_ge.h). The command ids, sizes
 * and layouts were checked against the stock libged.so call sites. The
 * structures have the same layout for 32-bit and 64-bit callers; only
 * struct GED_BRIDGE_PACKAGE differs.
 */

#ifndef _MTK_GED_ABI_H
#define _MTK_GED_ABI_H

#include <linux/ioctl.h>
#include <linux/types.h>

typedef int GED_ERROR;

#define GED_OK				0
#define GED_ERROR_FAIL			1
#define GED_ERROR_OOM			2
#define GED_ERROR_OUT_OF_FD		3
#define GED_ERROR_FAIL_WITH_LIMIT	4
#define GED_ERROR_TIMEOUT		5
#define GED_ERROR_CMD_NOT_PROCESSED	6
#define GED_ERROR_INVALID_PARAMS	7
#define GED_ERROR_INTENTIONAL_BLOCK	8

struct GED_BRIDGE_PACKAGE {
	__u32 ui32FunctionID;
	__s32 i32Size;
	void *pvParamIn;
	__s32 i32InBufferSize;
	void *pvParamOut;
	__s32 i32OutBufferSize;
};

#define GED_MAGIC			'g'
#define GED_IOWR(nr)			_IOWR(GED_MAGIC, nr, struct GED_BRIDGE_PACKAGE)
#define GED_GET_BRIDGE_ID(cmd)		_IOC_NR(cmd)

#define GED_BRIDGE_COMMAND_LOG_BUF_GET			0
#define GED_BRIDGE_COMMAND_LOG_BUF_WRITE		1
#define GED_BRIDGE_COMMAND_LOG_BUF_RESET		2
#define GED_BRIDGE_COMMAND_BOOST_GPU_FREQ		3
#define GED_BRIDGE_COMMAND_MONITOR_3D_FENCE		4
#define GED_BRIDGE_COMMAND_QUERY_INFO			5
#define GED_BRIDGE_COMMAND_NOTIFY_VSYNC			6
#define GED_BRIDGE_COMMAND_DVFS_PROBE			7
#define GED_BRIDGE_COMMAND_DVFS_UM_RETURN		8
#define GED_BRIDGE_COMMAND_EVENT_NOTIFY			9
#define GED_BRIDGE_COMMAND_WAIT_HW_VSYNC		10
#define GED_BRIDGE_COMMAND_QUERY_TARGET_FPS		11
#define GED_BRIDGE_COMMAND_VSYNC_WAIT			12
#define GED_BRIDGE_COMMAND_GPU_HINT_TO_CPU		13
#define GED_BRIDGE_COMMAND_HINT_FORCE_MDP		14
#define GED_BRIDGE_COMMAND_QUERY_DVFS_FREQ_PRED		15
#define GED_BRIDGE_COMMAND_QUERY_GPU_DVFS_INFO		16
#define GED_BRIDGE_COMMAND_GE_ALLOC			100
#define GED_BRIDGE_COMMAND_GE_GET			101
#define GED_BRIDGE_COMMAND_GE_SET			102
#define GED_BRIDGE_COMMAND_GPU_TIMESTAMP		103
#define GED_BRIDGE_COMMAND_TARGET_FPS			104
#define GED_BRIDGE_COMMAND_GE_INFO			105
#define GED_BRIDGE_COMMAND_GPU_TUNER_STATUS		106
/* Issued by libged but absent from the stock MT6765 kernel. */
#define GED_BRIDGE_COMMAND_DMABUF_SET_NAME		107
#define GED_BRIDGE_COMMAND_CREATE_TIMELINE		200

#define GED_LOG_BUF_NAME_LENGTH		64
#define GED_BRIDGE_IN_LOGBUF_SIZE	320
#define GPU_TUNER_BUF_NAME_LEN		128
#define GED_DMABUF_NAME_LEN		48

/* gralloc_extra regions: ge_misc.cpp always allocates 18 of them. */
#define GE_ALLOC_STRUCT_NUM		18
#define GE_MAX_REGION_SIZE		8192

struct GED_BRIDGE_IN_LOGBUFGET {
	char acName[GED_LOG_BUF_NAME_LENGTH];
};

struct GED_BRIDGE_OUT_LOGBUFGET {
	GED_ERROR eError;
	__u32 hLogBuf;
};

struct GED_BRIDGE_IN_LOGBUFWRITE {
	__u32 hLogBuf;
	int attrs;
	char acLogBuf[GED_BRIDGE_IN_LOGBUF_SIZE];
};

struct GED_BRIDGE_OUT_LOGBUFWRITE {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_LOGBUFRESET {
	__u32 hLogBuf;
};

struct GED_BRIDGE_OUT_LOGBUFRESET {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_BOOSTGPUFREQ {
	int eGPUFreqLevel;
};

struct GED_BRIDGE_OUT_BOOSTGPUFREQ {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_MONITOR3DFENCE {
	int fd;
};

struct GED_BRIDGE_OUT_MONITOR3DFENCE {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_QUERY_INFO {
	int eType;
};

struct GED_BRIDGE_OUT_QUERY_INFO {
	__u64 retrieve;
};

struct GED_DVFS_UM_QUERY_PACK {
	char bFirstBorn;
	unsigned int ui32GPULoading;
	unsigned int ui32GPUFreqID;
	unsigned int gpu_cur_freq;
	unsigned int gpu_pre_freq;
	long long usT;
	long long nsOffset;
	unsigned long long ul3DFenceDoneTime;
	unsigned long long ulPreCalResetTS_us;
	unsigned long long ulWorkingPeriod_us;
	unsigned int ui32TargetPeriod_us;
	unsigned int ui32BoostValue;
};

struct GED_BRIDGE_IN_NOTIFY_VSYNC {
	int eType;
};

struct GED_BRIDGE_OUT_NOTIFY_VSYNC {
	struct GED_DVFS_UM_QUERY_PACK sQueryData;
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_DVFS_PROBE {
	int pid;
};

struct GED_BRIDGE_OUT_DVFS_PROBE {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_DVFS_UM_RETURN {
	__u64 gpu_tar_freq;
	__u8 bFallback;
};

struct GED_BRIDGE_OUT_DVFS_UM_RETURN {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_EVENT_NOTIFY {
	int eEvent;
	__u8 bSwitch;
};

struct GED_BRIDGE_OUT_EVENT_NOTIFY {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_GPU_TIMESTAMP {
	int pid;
	__u64 ullWnd;
	__s32 i32FrameID;
	int fence_fd;
	int QedBuffer_length;
	int isSF;
};

struct GED_BRIDGE_OUT_GPU_TIMESTAMP {
	GED_ERROR eError;
	int is_ged_kpi_enabled;
};

struct GED_BRIDGE_IN_GPU_HINT_TO_CPU {
	__s32 i32BridgeFD;
	__s32 tid;
	__s32 hint;
};

struct GED_BRIDGE_OUT_GPU_HINT_TO_CPU {
	GED_ERROR eError;
	__s32 boost_flag;
	__s32 boost_value;
};

struct GED_BRIDGE_IN_HINT_FORCE_MDP {
	__s32 i32BridgeFD;
	__s32 hint;
};

struct GED_BRIDGE_OUT_HINT_FORCE_MDP {
	GED_ERROR eError;
	__s32 mdp_flag;
};

struct GED_BRIDGE_IN_QUERY_DVFS_FREQ_PRED {
	__s32 pid;
	__s32 hint;
};

struct GED_BRIDGE_OUT_QUERY_DVFS_FREQ_PRED {
	GED_ERROR eError;
	int gpu_freq_cur;
	int gpu_freq_max;
	int gpu_freq_dvfs_pred;
};

struct GED_BRIDGE_IN_QUERY_GPU_DVFS_INFO {
	__s32 pid;
	__s32 hint;
	__s32 gift_ratio;
};

struct GED_BRIDGE_OUT_QUERY_GPU_DVFS_INFO {
	GED_ERROR eError;
	int gpu_freq_cur;
	int gpu_freq_max;
	int gpu_freq_dvfs_pred;
	int target_fps;
	int target_fps_margin;
	int eara_fps_margin;
	int gpu_time;
};

struct GED_BRIDGE_IN_GE_ALLOC {
	int region_num;
	__u32 region_sizes[];
};

struct GED_BRIDGE_OUT_GE_ALLOC {
	int ge_fd;
	GED_ERROR eError;
};

/* Offsets and sizes are in units of 32-bit words. */
struct GED_BRIDGE_IN_GE_GET {
	int ge_fd;
	int region_id;
	int uint32_offset;
	int uint32_size;
};

struct GED_BRIDGE_OUT_GE_GET {
	GED_ERROR eError;
	__u32 data[];
};

struct GED_BRIDGE_IN_GE_SET {
	int ge_fd;
	int region_id;
	int uint32_offset;
	int uint32_size;
	__u32 data[];
};

struct GED_BRIDGE_OUT_GE_SET {
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_GE_INFO {
	int ge_fd;
};

struct GED_BRIDGE_OUT_GE_INFO {
	__u64 unique_id;
	GED_ERROR eError;
};

struct GED_BRIDGE_IN_GPU_TUNER_STATUS {
	char name[GPU_TUNER_BUF_NAME_LEN];
};

struct GED_BRIDGE_OUT_GPU_TUNER_STATUS {
	int feature;
};

struct GED_BRIDGE_IN_DMABUF_SET_NAME {
	int fd;
	char name[GED_DMABUF_NAME_LEN];
};

struct GED_BRIDGE_OUT_DMABUF_SET_NAME {
	GED_ERROR eError;
};

#endif /* _MTK_GED_ABI_H */
