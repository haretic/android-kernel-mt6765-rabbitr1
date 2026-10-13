/*
 * r1-ion-ged-test: exercise the legacy MediaTek /dev/ion and /proc/ged compatibility drivers
 * the way the stock vendor blobs do (libion legacy path, libion_mtk custom ioctls, libged GE calls).
 *
 * Build from the kernel tree root, static, for both ABIs the blobs use:
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -Idrivers/staging/mtk-ion-compat -Idrivers/staging/mtk-ged \
 *       -o r1-ion-ged-test rabbit-r1/tools/r1-ion-ged-test.c
 *   arm-linux-gnueabihf-gcc -static -O2 -Wall -Idrivers/staging/mtk-ion-compat -Idrivers/staging/mtk-ged \
 *       -o r1-ion-ged-test32 rabbit-r1/tools/r1-ion-ged-test.c
 * Run as root (or a user allowed to open /dev/ion). Exit status is the number of failed checks.
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>

#include "mtk_ion_abi.h"
#include "ged_abi.h"

/* Layouts the stock blobs were disassembled to use (see the driver commits). */
#if defined(__LP64__)
_Static_assert(sizeof(struct ion_allocation_data) == 32, "alloc");
_Static_assert(sizeof(struct ion_custom_data) == 16, "custom");
_Static_assert(sizeof(struct ion_sys_data) == 120, "sys");
_Static_assert(offsetof(struct ion_sys_data, get_phys_param.phy_addr) == 16, "phys");
_Static_assert(offsetof(struct ion_mm_data, buf_debug_info_param.dbg_name) == 16, "dbg");
_Static_assert(offsetof(struct ion_mm_data, pool_info_param.ret) == 32, "pool");
_Static_assert(sizeof(struct GED_BRIDGE_PACKAGE) == 40, "pkg");
#else
_Static_assert(sizeof(struct ion_allocation_data) == 20, "alloc");
_Static_assert(sizeof(struct ion_custom_data) == 8, "custom");
_Static_assert(sizeof(struct ion_sys_data) == 96, "sys");
_Static_assert(offsetof(struct ion_sys_data, get_phys_param.phy_addr) == 12, "phys");
_Static_assert(offsetof(struct ion_mm_data, buf_debug_info_param.dbg_name) == 12, "dbg");
_Static_assert(offsetof(struct ion_mm_data, pool_info_param.ret) == 24, "pool");
_Static_assert(sizeof(struct GED_BRIDGE_PACKAGE) == 24, "pkg");
#endif
_Static_assert(sizeof(struct ion_mm_data) == 80, "mm");
_Static_assert(sizeof(struct GED_BRIDGE_OUT_NOTIFY_VSYNC) == 80, "vsync");
_Static_assert(sizeof(struct GED_BRIDGE_OUT_GE_INFO) == 16, "geinfo");

static int failures;

#define CHECK(cond, ...) do {						\
	if (cond) {							\
		printf("PASS ");					\
	} else {							\
		printf("FAIL ");					\
		failures++;						\
	}								\
	printf(__VA_ARGS__);						\
	printf("\n");							\
} while (0)

#define HEAP_MM_MASK		(1u << MTK_ION_HEAP_MULTIMEDIA)
#define HEAP_CAMERA_MASK	(1u << MTK_ION_HEAP_MULTIMEDIA_FOR_CAMERA)
#define HEAP_CONTIG_MASK	(1u << MTK_ION_HEAP_SYSTEM_CONTIG)
#define BUF_LEN			(256 * 1024 + 4096)
/* MT6765 DRAM starts here; M4U addresses for these ports sit below it. */
#define DRAM_BASE		0x40000000UL

static int ion_alloc(int fd, size_t len, unsigned int mask, unsigned int flags, int *handle)
{
	struct ion_allocation_data d = { .len = len, .align = 4096, .heap_id_mask = mask, .flags = flags };

	if (ioctl(fd, ION_IOC_ALLOC, &d) < 0)
		return -errno;
	*handle = d.handle;
	return 0;
}

static int ion_free(int fd, int handle)
{
	struct ion_handle_data d = { .handle = handle };

	return ioctl(fd, ION_IOC_FREE, &d) < 0 ? -errno : 0;
}

static int ion_fd_op(int fd, unsigned long req, int handle, int *share_fd)
{
	struct ion_fd_data d = { .handle = handle };

	if (ioctl(fd, req, &d) < 0)
		return -errno;
	*share_fd = d.fd;
	return 0;
}

static int ion_import(int fd, int share_fd, int *handle)
{
	struct ion_fd_data d = { .fd = share_fd };

	if (ioctl(fd, ION_IOC_IMPORT, &d) < 0)
		return -errno;
	*handle = d.handle;
	return 0;
}

static int ion_custom(int fd, unsigned int cmd, void *arg)
{
	struct ion_custom_data d = { .cmd = cmd, .arg = (unsigned long)arg };

	return ioctl(fd, ION_IOC_CUSTOM, &d) < 0 ? -errno : 0;
}

static int fdinfo_has(int fd, const char *needle)
{
	char path[64], buf[4096];
	ssize_t n;
	int f;

	snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
	f = open(path, O_RDONLY);
	if (f < 0)
		return 0;
	n = read(f, buf, sizeof(buf) - 1);
	close(f);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strstr(buf, needle) != NULL;
}

static void test_ion(int *shared_fd_out)
{
	struct ion_heap_query q = { 0 };
	struct ion_heap_data heaps[32];
	struct ion_sys_data sys;
	struct ion_mm_data mm;
	int ion, ion2, h, h2, h3, hc, fd, fd2, ret;
	uint32_t *p, *p2;
	unsigned int i;

	*shared_fd_out = -1;
	ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	CHECK(ion >= 0, "open /dev/ion (%s)", ion < 0 ? strerror(errno) : "ok");
	if (ion < 0)
		return;

	/* libion's ion_is_legacy(): FREE(0) must fail, but not with ENOTTY. */
	ret = ion_free(ion, 0);
	CHECK(ret < 0 && ret != -ENOTTY, "legacy probe ION_IOC_FREE(0) -> %d", ret);

	/* libion_mtk ion_set_client_name() */
	memset(&sys, 0, sizeof(sys));
	sys.sys_cmd = ION_SYS_SET_CLIENT_NAME;
	strncpy(sys.client_name_param.name, "r1-ion-ged-test", ION_MM_DBG_NAME_LEN - 1);
	CHECK(ion_custom(ion, ION_CMD_SYSTEM, &sys) == 0, "ION_SYS_SET_CLIENT_NAME");

	ret = ioctl(ion, ION_IOC_HEAP_QUERY, &q);
	CHECK(ret == 0 && q.cnt > 0, "ION_IOC_HEAP_QUERY count %u", q.cnt);
	if (q.cnt > 32)
		q.cnt = 32;
	q.heaps = (uintptr_t)heaps;
	ret = ioctl(ion, ION_IOC_HEAP_QUERY, &q);
	CHECK(ret == 0, "ION_IOC_HEAP_QUERY list");
	for (i = 0; !ret && i < q.cnt; i++)
		printf("     heap id %2u type %2u %s\n", heaps[i].heap_id, heaps[i].type, heaps[i].name);

	CHECK(ion_alloc(ion, 4096, 0, 0, &h) < 0, "ION_IOC_ALLOC with empty heap mask fails");

	/* gralloc / libion_mtk ion_alloc_mm(): uncached multimedia heap */
	ret = ion_alloc(ion, BUF_LEN, HEAP_MM_MASK, 0, &h);
	CHECK(ret == 0 && h > 0, "ION_IOC_ALLOC mm heap -> handle %d (%d)", h, ret);
	if (ret)
		goto out;

	ret = ion_fd_op(ion, ION_IOC_SHARE, h, &fd);
	CHECK(ret == 0 && fd >= 0, "ION_IOC_SHARE -> fd %d", fd);
	if (ret)
		goto out;
	CHECK(fdinfo_has(fd, "mtk_ion_compat"), "share fd is a mtk_ion_compat dma-buf");
	CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "share fd is close-on-exec");

	p = mmap(NULL, BUF_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED, "mmap share fd");
	if (p == MAP_FAILED)
		goto out;
	for (i = 0; i < BUF_LEN / 4 && !p[i]; i++)
		;
	CHECK(i == BUF_LEN / 4, "fresh buffer is zeroed");
	for (i = 0; i < BUF_LEN / 4; i++)
		p[i] = 0x5a000000u | i;

	ret = ion_fd_op(ion, ION_IOC_MAP, h, &fd2);
	CHECK(ret == 0 && fd2 >= 0 && fd2 != fd, "ION_IOC_MAP -> fd %d", fd2);
	if (!ret)
		close(fd2);

	/* Importing a buffer the client already holds returns the same handle. */
	ret = ion_import(ion, fd, &h2);
	CHECK(ret == 0 && h2 == h, "ION_IOC_IMPORT own buffer -> handle %d (want %d)", h2, h);
	CHECK(ion_free(ion, h2) == 0, "ION_IOC_FREE import reference");

	/* MediaTek debug info round trip (gralloc_extra ge_perform/query_ion_debug) */
	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_SET_DEBUG_INFO;
	mm.buf_debug_info_param.handle = h;
	strcpy(mm.buf_debug_info_param.dbg_name, "r1-test-buffer");
	mm.buf_debug_info_param.value1 = 11;
	mm.buf_debug_info_param.value4 = 44;
	CHECK(ion_custom(ion, ION_CMD_MULTIMEDIA, &mm) == 0, "ION_MM_SET_DEBUG_INFO");
	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_GET_DEBUG_INFO;
	mm.buf_debug_info_param.handle = h;
	ret = ion_custom(ion, ION_CMD_MULTIMEDIA, &mm);
	CHECK(ret == 0 && !strcmp(mm.buf_debug_info_param.dbg_name, "r1-test-buffer") &&
	      mm.buf_debug_info_param.value1 == 11 && mm.buf_debug_info_param.value4 == 44,
	      "ION_MM_GET_DEBUG_INFO -> '%s' %u %u", mm.buf_debug_info_param.dbg_name,
	      mm.buf_debug_info_param.value1, mm.buf_debug_info_param.value4);
	CHECK(fdinfo_has(fd, "r1-test-buffer"), "debug name is the dma-buf name");

	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_CONFIG_BUFFER;
	mm.config_buffer_param.handle = h;
	mm.config_buffer_param.module_id = 0;
	CHECK(ion_custom(ion, ION_CMD_MULTIMEDIA, &mm) == 0, "ION_MM_CONFIG_BUFFER");

	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_GET_IOVA;
	mm.get_phys_param.handle = h;
	CHECK(ion_custom(ion, ION_CMD_MULTIMEDIA, &mm) < 0,
	      "ION_MM_GET_IOVA fails cleanly for a port no driver serves");

	/* libion_mtk ion_alloc_camera_pool(ION_MM_QRY_CACHE_POOL) */
	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_QRY_CACHE_POOL;
	mm.pool_info_param.len = 4096;
	mm.pool_info_param.heap_id_mask = HEAP_CAMERA_MASK;
	mm.pool_info_param.ret = 1234;
	ret = ion_custom(ion, ION_CMD_MULTIMEDIA, &mm);
	CHECK(ret == 0 && mm.pool_info_param.ret == 0, "ION_MM_QRY_CACHE_POOL -> %u", mm.pool_info_param.ret);

	memset(&sys, 0, sizeof(sys));
	sys.sys_cmd = ION_SYS_CACHE_SYNC;
	sys.cache_sync_param.handle = h;
	sys.cache_sync_param.sync_type = ION_CACHE_FLUSH_BY_RANGE;
	CHECK(ion_custom(ion, ION_CMD_SYSTEM, &sys) == 0, "ION_SYS_CACHE_SYNC flush");

	memset(&sys, 0, sizeof(sys));
	sys.sys_cmd = ION_SYS_GET_PHYS;
	sys.get_phys_param.handle = h;
	/* Without a port there is no M4U address: an error for a scattered buffer,
	 * the physical address for a contiguous one (the default when it can be had). */
	ret = ion_custom(ion, ION_CMD_SYSTEM, &sys);
	CHECK(ret < 0 || sys.get_phys_param.phy_addr >= DRAM_BASE,
	      "ION_SYS_GET_PHYS: no M4U address without a port (%d, %#llx)", ret,
	      (unsigned long long)sys.get_phys_param.phy_addr);

	{
		struct ion_fd_data d = { .fd = fd };

		CHECK(ioctl(ion, ION_IOC_SYNC, &d) == 0, "ION_IOC_SYNC");
	}
	{
		struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };

		CHECK(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA_BUF_IOCTL_SYNC start");
		s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
		CHECK(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA_BUF_IOCTL_SYNC end");
	}

	/* Contiguous heap: GET_PHYS answers. */
	ret = ion_alloc(ion, 64 * 1024, HEAP_CONTIG_MASK, ION_FLAG_CACHED, &hc);
	CHECK(ret == 0, "ION_IOC_ALLOC system-contig heap (%d)", ret);
	if (!ret) {
		memset(&sys, 0, sizeof(sys));
		sys.sys_cmd = ION_SYS_GET_PHYS;
		sys.get_phys_param.handle = hc;
		ret = ion_custom(ion, ION_CMD_SYSTEM, &sys);
		CHECK(ret == 0 && sys.get_phys_param.phy_addr && sys.get_phys_param.len == 64 * 1024,
		      "ION_SYS_GET_PHYS contig -> %#lx + %lu", sys.get_phys_param.phy_addr,
		      sys.get_phys_param.len);
		CHECK(ion_free(ion, hc) == 0, "ION_IOC_FREE contig");
	}

	/* A second client (another process in real life) imports the shared fd. */
	ion2 = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	CHECK(ion2 >= 0, "open /dev/ion again");
	if (ion2 >= 0) {
		ret = ion_import(ion2, fd, &h3);
		CHECK(ret == 0 && h3 > 0, "second client ION_IOC_IMPORT -> handle %d", h3);
		if (!ret) {
			ret = ion_fd_op(ion2, ION_IOC_SHARE, h3, &fd2);
			CHECK(ret == 0, "second client ION_IOC_SHARE");
			if (!ret) {
				p2 = mmap(NULL, BUF_LEN, PROT_READ, MAP_SHARED, fd2, 0);
				CHECK(p2 != MAP_FAILED && p2[0] == 0x5a000000u &&
				      p2[BUF_LEN / 4 - 1] == (0x5a000000u | (BUF_LEN / 4 - 1)),
				      "second client sees the data");
				if (p2 != MAP_FAILED)
					munmap(p2, BUF_LEN);
				close(fd2);
			}
			CHECK(ion_free(ion2, h3) == 0, "second client ION_IOC_FREE");
			CHECK(ion_free(ion2, h3) < 0, "double ION_IOC_FREE fails");
		}
		close(ion2);
	}

	munmap(p, BUF_LEN);
	CHECK(ion_free(ion, h) == 0, "ION_IOC_FREE allocation handle");
	/* The dma-buf fd keeps the memory alive after the handle is gone. */
	*shared_fd_out = fd;
out:
	close(ion);
}

static int ged_call(int ged, unsigned int id, void *in, int in_size, void *out, int out_size)
{
	struct GED_BRIDGE_PACKAGE pkg = {
		.ui32FunctionID = GED_IOWR(id),
		.i32Size = sizeof(pkg),
		.pvParamIn = in,
		.i32InBufferSize = in_size,
		.pvParamOut = out,
		.i32OutBufferSize = out_size,
	};

	return ioctl(ged, GED_IOWR(id), &pkg) < 0 ? -errno : 0;
}

static void test_ged(int dmabuf_fd)
{
	struct {
		struct GED_BRIDGE_IN_GE_ALLOC hdr;
		uint32_t sizes[GE_ALLOC_STRUCT_NUM];
	} alloc_in;
	struct GED_BRIDGE_OUT_GE_ALLOC alloc_out = { -1, 1 };
	struct {
		struct GED_BRIDGE_IN_GE_SET hdr;
		uint32_t data[6];
	} set_in;
	struct GED_BRIDGE_OUT_GE_SET set_out;
	struct GED_BRIDGE_IN_GE_GET get_in;
	struct {
		struct GED_BRIDGE_OUT_GE_GET hdr;
		uint32_t data[6];
	} get_out;
	struct GED_BRIDGE_IN_GE_INFO info_in;
	struct GED_BRIDGE_OUT_GE_INFO info_out;
	struct GED_BRIDGE_IN_LOGBUFGET lg_in = { "HWC" };
	struct GED_BRIDGE_OUT_LOGBUFGET lg_out;
	struct GED_BRIDGE_IN_LOGBUFWRITE lw_in = { 0 };
	struct GED_BRIDGE_OUT_LOGBUFWRITE lw_out;
	struct GED_BRIDGE_IN_NOTIFY_VSYNC nv_in = { 0 };
	struct GED_BRIDGE_OUT_NOTIFY_VSYNC nv_out;
	struct GED_BRIDGE_IN_DMABUF_SET_NAME sn_in;
	struct GED_BRIDGE_OUT_DMABUF_SET_NAME sn_out;
	int ged, ret, i;

	ged = open("/proc/ged", O_RDONLY);
	CHECK(ged >= 0, "open /proc/ged (%s)", ged < 0 ? strerror(errno) : "ok");
	if (ged < 0)
		return;

	alloc_in.hdr.region_num = GE_ALLOC_STRUCT_NUM;
	for (i = 0; i < GE_ALLOC_STRUCT_NUM; i++)
		alloc_in.sizes[i] = 4 * (i + 4);
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_ALLOC, &alloc_in, sizeof(alloc_in),
		       &alloc_out, sizeof(alloc_out));
	CHECK(ret == 0 && alloc_out.eError == GED_OK && alloc_out.ge_fd >= 0,
	      "GE_ALLOC -> fd %d", alloc_out.ge_fd);
	if (ret || alloc_out.ge_fd < 0)
		goto out;

	get_in = (struct GED_BRIDGE_IN_GE_GET){ alloc_out.ge_fd, 3, 0, 6 };
	memset(&get_out, 0xff, sizeof(get_out));
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_GET, &get_in, sizeof(get_in), &get_out, sizeof(get_out));
	CHECK(ret == 0 && get_out.hdr.eError == 0 && get_out.data[0] == 0 && get_out.data[5] == 0,
	      "GE_GET of an unset region reads zeros");

	set_in.hdr = (struct GED_BRIDGE_IN_GE_SET){ alloc_out.ge_fd, 3, 0, 6 };
	for (i = 0; i < 6; i++)
		set_in.data[i] = 0xc0de0000u + i;
	set_out.eError = 1;
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_SET, &set_in, sizeof(set_in), &set_out, sizeof(set_out));
	CHECK(ret == 0 && set_out.eError == 0, "GE_SET region 3 (6 words)");

	memset(&get_out, 0, sizeof(get_out));
	get_out.hdr.eError = 1;
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_GET, &get_in, sizeof(get_in), &get_out, sizeof(get_out));
	CHECK(ret == 0 && get_out.hdr.eError == 0 && get_out.data[0] == 0xc0de0000u &&
	      get_out.data[5] == 0xc0de0005u, "GE_GET reads back what GE_SET wrote");

	/* Region 0 is 16 bytes: 6 words do not fit. */
	set_in.hdr.region_id = 0;
	set_out.eError = 0;
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_SET, &set_in, sizeof(set_in), &set_out, sizeof(set_out));
	CHECK(ret == 0 && set_out.eError != 0, "GE_SET past the region end is refused (%d)", set_out.eError);

	/* Output size must be header + requested words exactly. */
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_GET, &get_in, sizeof(get_in), &get_out, sizeof(get_out) - 4);
	CHECK(ret < 0, "GE_GET with a short output buffer fails");

	info_in.ge_fd = alloc_out.ge_fd;
	memset(&info_out, 0xff, sizeof(info_out));
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_INFO, &info_in, sizeof(info_in), &info_out, sizeof(info_out));
	CHECK(ret == 0 && info_out.eError == 0, "GE_INFO (ged_ge_query) -> unique id %llu",
	      (unsigned long long)info_out.unique_id);

	info_in.ge_fd = ged;
	ret = ged_call(ged, GED_BRIDGE_COMMAND_GE_INFO, &info_in, sizeof(info_in), &info_out, sizeof(info_out));
	CHECK(ret < 0, "GE_INFO on a non-GE fd fails");
	close(alloc_out.ge_fd);

	ret = ged_call(ged, GED_BRIDGE_COMMAND_LOG_BUF_GET, &lg_in, sizeof(lg_in), &lg_out, sizeof(lg_out));
	CHECK(ret == 0 && lg_out.eError == 0 && lg_out.hLogBuf, "LOG_BUF_GET HWC -> %u", lg_out.hLogBuf);
	lw_in.hLogBuf = lg_out.hLogBuf;
	lw_in.attrs = 0x200;
	strcpy(lw_in.acLogBuf, "r1-ion-ged-test log line");
	ret = ged_call(ged, GED_BRIDGE_COMMAND_LOG_BUF_WRITE, &lw_in, sizeof(lw_in), &lw_out, sizeof(lw_out));
	CHECK(ret == 0 && lw_out.eError == 0, "LOG_BUF_WRITE");

	ret = ged_call(ged, GED_BRIDGE_COMMAND_NOTIFY_VSYNC, &nv_in, sizeof(nv_in), &nv_out, sizeof(nv_out));
	CHECK(ret == 0 && nv_out.eError == GED_ERROR_INTENTIONAL_BLOCK, "NOTIFY_VSYNC -> eError %d", nv_out.eError);

	if (dmabuf_fd >= 0) {
		memset(&sn_in, 0, sizeof(sn_in));
		sn_in.fd = dmabuf_fd;
		strcpy(sn_in.name, "r1-ged-named");
		sn_out.eError = 1;
		ret = ged_call(ged, GED_BRIDGE_COMMAND_DMABUF_SET_NAME, &sn_in, sizeof(sn_in),
			       &sn_out, sizeof(sn_out));
		CHECK(ret == 0 && sn_out.eError == 0 && fdinfo_has(dmabuf_fd, "r1-ged-named"),
		      "DMABUF_SET_NAME (ged_dmabuf_set_name)");
	}

	ret = ioctl(ged, 0x5401 /* TCGETS */, &ret);
	CHECK(ret < 0 && errno == ENOTTY, "non-GED ioctl gets ENOTTY");
out:
	close(ged);
}

int main(void)
{
	int fd;

	printf("r1-ion-ged-test (%zu-bit)\n", sizeof(void *) * 8);
	test_ion(&fd);
	test_ged(fd);
	if (fd >= 0)
		close(fd);
	printf("%d failure(s)\n", failures);
	return failures;
}
