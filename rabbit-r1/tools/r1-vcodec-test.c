/*
 * r1-vcodec-test: exercise the MT6765 /dev/Vcodec driver (mtk-vcodec-compat) the way the stock
 * codec libraries do: libvcodec_utility's eValInit/eVideoLockHW/eVideoWaitHWDone/eHalInit mmaps,
 * and eVideoMemAlloc's M4U addresses through /dev/ion (ION_MM_CONFIG_BUFFER + ION_SYS_GET_PHYS),
 * for ION buffers and for the DMA heap buffers Codec2 hands the libraries.
 *
 * Build from the kernel tree root, static, for both ABIs (the codec libraries are 32-bit, so
 * r1-vcodec-test32 is the one that matches them):
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -Idrivers/staging/mtk-vcodec-compat \
 *       -Idrivers/staging/mtk-ion-compat -o r1-vcodec-test rabbit-r1/tools/r1-vcodec-test.c
 *   arm-linux-gnueabihf-gcc -static -O2 -Wall -Idrivers/staging/mtk-vcodec-compat \
 *       -Idrivers/staging/mtk-ion-compat -o r1-vcodec-test32 rabbit-r1/tools/r1-vcodec-test.c
 * Run as root with nothing else using the codec (stop vendor.media.c2-hal-1-2 first).
 * It never starts an engine, so interrupt waits are only checked to time out.
 * Exit status is the number of failed checks.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mtk_vcodec_abi.h"
#include "mtk_ion_abi.h"

/* The caller's own VAL_HW_LOCK_T / VAL_ISR_T, as the blobs of this ABI lay them out. */
#if defined(__LP64__)
typedef struct vcodec_hw_lock hw_lock_t;
typedef struct vcodec_isr isr_t;
_Static_assert(sizeof(hw_lock_t) == 56, "lock");
_Static_assert(sizeof(isr_t) == 112, "isr");
#else
typedef struct vcodec_hw_lock32 hw_lock_t;
typedef struct vcodec_isr32 isr_t;
_Static_assert(sizeof(hw_lock_t) == 32, "lock");
_Static_assert(sizeof(isr_t) == 96, "isr");
#endif

#define VENC_BASE		0x17020000UL
#define VDEC_BASE		0x17040000UL
#define GCON_BASE		0x17000000UL
#define GCON_VDEC_ACTIVE	0x10
#define GCON_VCODEC_SEL		0x20
#define GCON_VENC_DEBLOCK	0x24
#define VENC_IRQ_STATUS		0x5c

/* Stock M4U port ids: VENC_RCPU (eVencPortID) and VENC_CUR_LUMA, served by the codec. */
#define PORT_VENC_RCPU		8
#define PORT_VENC_CUR_LUMA	15
/* JPGENC_RDMA: larb1, but no driver here registers it. */
#define PORT_JPGENC_RDMA	13
#define HEAP_MM_MASK		(1u << MTK_ION_HEAP_MULTIMEDIA)
#define BUF_LEN			(256 * 1024 + 4096)
/* MT6765 DRAM starts here; M4U addresses for these ports sit below it. */
#define DRAM_BASE		0x40000000UL

/* <linux/dma-heap.h> */
struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC	_IOWR('H', 0x0, struct dma_heap_allocation_data)

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

static int xioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg) < 0 ? -errno : 0;
}

static int lock_hw(int fd, uintptr_t handle, unsigned int type, unsigned int timeout_ms)
{
	hw_lock_t l;

	memset(&l, 0, sizeof(l));
	l.handle = handle;
	l.timeout_ms = timeout_ms;
	l.driver_type = type;
	return xioctl(fd, VCODEC_LOCKHW, &l);
}

static int unlock_hw(int fd, uintptr_t handle, unsigned int type)
{
	hw_lock_t l;

	memset(&l, 0, sizeof(l));
	l.handle = handle;
	l.driver_type = type;
	return xioctl(fd, VCODEC_UNLOCKHW, &l);
}

static int wait_isr(int fd, uintptr_t handle, unsigned int type, unsigned int timeout_ms)
{
	isr_t isr;

	memset(&isr, 0, sizeof(isr));
	isr.handle = handle;
	isr.driver_type = type;
	isr.timeout_ms = timeout_ms;
	isr.irq_status_num = 1;
	return xioctl(fd, VCODEC_WAITISR, &isr);
}

static long elapsed_ms(const struct timespec *from)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - from->tv_sec) * 1000 + (now.tv_nsec - from->tv_nsec) / 1000000;
}

static volatile uint32_t *map_regs(int fd, unsigned long phys, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)phys);

	return p == MAP_FAILED ? NULL : p;
}

static void test_misc(int a, int b)
{
	unsigned int count, base;
	int n = 0;

	CHECK(xioctl(a, _IOW(MFV_IOC_MAGIC, 0x7f, unsigned int), &n) == -ENOTTY,
	      "unknown command -> ENOTTY");
	CHECK(xioctl(a, VCODEC_INITHWLOCK, &n) == 0, "VCODEC_INITHWLOCK accepted");
	CHECK(xioctl(a, VCODEC_GET_CORE_NUMBER, &n) == 0 && n > 0,
	      "VCODEC_GET_CORE_NUMBER -> %d (possible CPUs; %ld configured)", n,
	      sysconf(_SC_NPROCESSORS_CONF));

	/* eValInit: the first encoder user (count 1) sets up EMI bandwidth. */
	CHECK(xioctl(a, VCODEC_INC_ENC_EMI_USER, &base) == 0 && base >= 1,
	      "VCODEC_INC_ENC_EMI_USER -> %u", base);
	CHECK(xioctl(b, VCODEC_INC_ENC_EMI_USER, &count) == 0 && count == base + 1,
	      "second user -> %u", count);
	CHECK(xioctl(b, VCODEC_DEC_ENC_EMI_USER, &count) == 0 && count == base,
	      "VCODEC_DEC_ENC_EMI_USER -> %u", count);
	CHECK(xioctl(a, VCODEC_DEC_ENC_EMI_USER, &count) == 0 && count == base - 1,
	      "last user gone -> %u", count);
}

static void test_mmap(int fd)
{
	static const struct { unsigned long phys; size_t len; int ok; const char *what; } w[] = {
		{ VENC_BASE, 0x1000, 1, "VENC (H264_GetHWRegBase)" },
		{ GCON_BASE, 0x100, 1, "vcodecsys GCON (eHalInit)" },
		{ VDEC_BASE, 0x400, 1, "VDEC VLD (eHalInit)" },
		{ VDEC_BASE + 0x5000, 0x200, 1, "VDEC MISC (eHalInit)" },
		{ VDEC_BASE + 0x8000, 0x1000, 1, "VDEC 0x8000 (eHalInit)" },
		{ GCON_BASE, 0x2000, 0, "vcodecsys beyond its first page" },
		{ 0x17010000UL, 0x1000, 0, "SMI larb1" },
		{ 0x17030000UL, 0x1000, 0, "JPEG encoder" },
		{ 0x10000000UL, 0x1000, 0, "topckgen" },
	};

	for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
		volatile uint32_t *p = map_regs(fd, w[i].phys, w[i].len);

		CHECK(!!p == w[i].ok, "mmap %#lx + %#zx, %s: %s", w[i].phys, w[i].len, w[i].what,
		      p ? "mapped" : strerror(errno));
		if (p)
			munmap((void *)p, w[i].len);
	}
}

static void test_lock(int a, int b)
{
	volatile uint32_t *gcon = map_regs(a, GCON_BASE, 0x100);
	volatile uint32_t *venc = map_regs(a, VENC_BASE, 0x1000);
	uintptr_t h1 = 0x1000, h2 = 0x2000;
	int ret;

	CHECK(gcon && venc, "register windows mapped");
	if (!gcon || !venc)
		return;

	ret = lock_hw(a, h1, VAL_DRIVER_TYPE_H264_ENC, 1000);
	CHECK(ret == 0, "LOCKHW H264_ENC -> %d", ret);
	if (ret)
		return;
	CHECK(gcon[GCON_VCODEC_SEL / 4] == 1 && gcon[GCON_VENC_DEBLOCK / 4] != 0 &&
	      gcon[GCON_VDEC_ACTIVE / 4] == 0, "encoder selected: SEL %#x DEBLOCK %#x VDEC %#x",
	      gcon[GCON_VCODEC_SEL / 4], gcon[GCON_VENC_DEBLOCK / 4], gcon[GCON_VDEC_ACTIVE / 4]);
	printf("info VENC IRQ_STATUS %#x\n", venc[VENC_IRQ_STATUS / 4]);

	CHECK(lock_hw(a, h1, VAL_DRIVER_TYPE_H264_ENC, 1000) == -EDEADLK,
	      "same instance locking twice -> EDEADLK");
	CHECK(lock_hw(b, h2, VAL_DRIVER_TYPE_H264_ENC, 0) == -EBUSY,
	      "other instance, timeout 0 -> EBUSY");
	ret = wait_isr(a, h1, VAL_DRIVER_TYPE_H264_ENC, 20);
	CHECK(ret == -ETIMEDOUT, "WAITISR on an idle encoder -> %d (ETIMEDOUT)", ret);
	CHECK(wait_isr(b, h2, VAL_DRIVER_TYPE_H264_ENC, 20) == -EPERM, "WAITISR by a non-owner -> EPERM");
	CHECK(unlock_hw(b, h2, VAL_DRIVER_TYPE_H264_ENC) == -EPERM, "UNLOCKHW by a non-owner -> EPERM");
	CHECK(unlock_hw(a, h1, VAL_DRIVER_TYPE_H264_ENC) == 0, "UNLOCKHW H264_ENC");
	CHECK(unlock_hw(a, h1, VAL_DRIVER_TYPE_H264_ENC) == -EPERM, "UNLOCKHW twice -> EPERM");

	ret = lock_hw(b, h2, VAL_DRIVER_TYPE_H264_DEC, 0);
	CHECK(ret == 0, "LOCKHW H264_DEC -> %d", ret);
	if (!ret) {
		CHECK(gcon[GCON_VCODEC_SEL / 4] == 2 && gcon[GCON_VDEC_ACTIVE / 4] == 1,
		      "decoder selected: SEL %#x VDEC %#x", gcon[GCON_VCODEC_SEL / 4],
		      gcon[GCON_VDEC_ACTIVE / 4]);
		ret = wait_isr(b, h2, VAL_DRIVER_TYPE_H264_DEC, 20);
		CHECK(ret == -ETIMEDOUT, "WAITISR on an idle decoder -> %d (ETIMEDOUT)", ret);
		CHECK(unlock_hw(b, h2, VAL_DRIVER_TYPE_H264_DEC) == 0, "UNLOCKHW H264_DEC");
	}

	ret = lock_hw(a, h1, VAL_DRIVER_TYPE_HEVC_DEC, 0);
	CHECK(ret == 0 && unlock_hw(a, h1, VAL_DRIVER_TYPE_HEVC_DEC) == 0, "HEVC_DEC lock/unlock");
	CHECK(lock_hw(a, h1, VAL_DRIVER_TYPE_JPEG_ENC, 0) == -EOPNOTSUPP, "JPEG_ENC -> EOPNOTSUPP");
	CHECK(lock_hw(a, h1, 0, 0) == -EINVAL, "driver type 0 -> EINVAL");
	CHECK(lock_hw(a, 0, VAL_DRIVER_TYPE_H264_ENC, 0) == -EINVAL, "handle 0 -> EINVAL");
	{
		hw_lock_t l;

		memset(&l, 0, sizeof(l));
		l.handle = h1;
		l.driver_type = VAL_DRIVER_TYPE_H264_DEC;
		l.secure_inst = 1;
		CHECK(xioctl(a, VCODEC_LOCKHW, &l) == -EOPNOTSUPP, "secure instance -> EOPNOTSUPP");
	}

	munmap((void *)gcon, 0x100);
	munmap((void *)venc, 0x1000);
}

/* A second process waits for the core: decoders for as long as it takes, encoders up to 30 s. */
static void test_contention(int a, unsigned int waiter_type)
{
	struct timespec start;
	int status = -1;
	pid_t pid;

	if (lock_hw(a, 0x1000, VAL_DRIVER_TYPE_H264_ENC, 0)) {
		CHECK(0, "LOCKHW for the contention test");
		return;
	}

	pid = fork();
	if (pid == 0) {
		int fd = open("/dev/Vcodec", O_RDWR | O_CLOEXEC);
		long ms;
		int ret;

		clock_gettime(CLOCK_MONOTONIC, &start);
		ret = fd < 0 ? -errno : lock_hw(fd, 0x3000, waiter_type, 1000);
		ms = elapsed_ms(&start);
		if (!ret)
			unlock_hw(fd, 0x3000, waiter_type);
		_exit(ret == 0 && ms >= 150 ? 0 : 1);
	}

	usleep(200 * 1000);
	unlock_hw(a, 0x1000, VAL_DRIVER_TYPE_H264_ENC);
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "type %u waits for the core and gets it when it is released", waiter_type);
}

/* The core comes back when its owner dies without unlocking. */
static void test_abandon(int a)
{
	int status = -1, ret;
	pid_t pid = fork();

	if (pid == 0) {
		int fd = open("/dev/Vcodec", O_RDWR | O_CLOEXEC);

		_exit(fd >= 0 && lock_hw(fd, 0x4000, VAL_DRIVER_TYPE_H264_ENC, 0) == 0 ? 0 : 1);
	}
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child took the core and exited");
	ret = lock_hw(a, 0x1000, VAL_DRIVER_TYPE_H264_ENC, 0);
	CHECK(ret == 0, "core released on close (dmesg: closed holding the core) -> %d", ret);
	if (!ret)
		unlock_hw(a, 0x1000, VAL_DRIVER_TYPE_H264_ENC);
}

static int ion_custom(int fd, unsigned int cmd, void *arg)
{
	struct ion_custom_data d = { .cmd = cmd, .arg = (unsigned long)arg };

	return xioctl(fd, ION_IOC_CUSTOM, &d);
}

static int ion_config(int ion, int handle, int port)
{
	struct ion_mm_data mm;

	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_CONFIG_BUFFER;
	mm.config_buffer_param.handle = handle;
	mm.config_buffer_param.module_id = port;
	return ion_custom(ion, ION_CMD_MULTIMEDIA, &mm);
}

static int ion_phys(int ion, int handle, unsigned long *addr, unsigned long *len)
{
	struct ion_sys_data sys;
	int ret;

	memset(&sys, 0, sizeof(sys));
	sys.sys_cmd = ION_SYS_GET_PHYS;
	sys.get_phys_param.handle = handle;
	ret = ion_custom(ion, ION_CMD_SYSTEM, &sys);
	*addr = sys.get_phys_param.phy_addr;
	*len = sys.get_phys_param.len;
	return ret;
}

/* eVideoMemAlloc: a scattered ION buffer gets one M4U address for the codec's ports. */
static void test_iova(void)
{
	struct ion_allocation_data alloc = {
		.len = BUF_LEN, .align = 4096, .heap_id_mask = HEAP_MM_MASK,
	};
	unsigned long addr, again, len, unserved;
	struct ion_mm_data mm;
	int ion, ret;

	ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	CHECK(ion >= 0, "open /dev/ion");
	if (ion < 0)
		return;
	ret = xioctl(ion, ION_IOC_ALLOC, &alloc);
	CHECK(ret == 0, "ION_IOC_ALLOC %d bytes -> %d", BUF_LEN, ret);
	if (ret)
		goto out;

	/*
	 * A port no driver serves gets no M4U mapping: GET_PHYS fails for a
	 * scattered buffer and returns the physical address of a contiguous one
	 * (the MM heap allocates contiguously when it can). Either way it must
	 * not be the larb1 IOVA checked below.
	 */
	unserved = 0;
	CHECK(ion_config(ion, alloc.handle, PORT_JPGENC_RDMA) == 0 &&
	      (ion_phys(ion, alloc.handle, &unserved, &len) < 0 || unserved >= DRAM_BASE),
	      "no M4U address for a port no driver serves (%#lx)", unserved);

	CHECK(ion_config(ion, alloc.handle, PORT_VENC_RCPU) == 0, "ION_MM_CONFIG_BUFFER VENC_RCPU");
	ret = ion_phys(ion, alloc.handle, &addr, &len);
	CHECK(ret == 0 && addr && !(addr & 0xfff) && (uint64_t)addr + len <= 1ULL << 32 &&
	      len == BUF_LEN && addr != unserved, "ION_SYS_GET_PHYS -> %d, IOVA %#lx + %#lx", ret, addr, len);
	ret = ion_phys(ion, alloc.handle, &again, &len);
	CHECK(ret == 0 && again == addr, "same IOVA on the next query (%#lx)", again);

	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_GET_IOVA;
	mm.get_phys_param.handle = alloc.handle;
	mm.get_phys_param.module_id = PORT_VENC_CUR_LUMA;
	ret = ion_custom(ion, ION_CMD_MULTIMEDIA, &mm);
	CHECK(ret == 0 && mm.get_phys_param.phy_addr == addr && mm.get_phys_param.len == BUF_LEN,
	      "ION_MM_GET_IOVA VENC_CUR_LUMA -> %d, the same mapping (%#llx)", ret,
	      (unsigned long long)mm.get_phys_param.phy_addr);

	{
		struct ion_handle_data d = { .handle = alloc.handle };

		CHECK(xioctl(ion, ION_IOC_FREE, &d) == 0, "ION_IOC_FREE (unmaps the IOVA)");
	}
out:
	close(ion);
}

/*
 * Whether debugfs lists a dma-buf with inode @ino: 1 or 0, or -1 if it cannot
 * be read (dma_buf_debug_show prints the inode as the sixth field, "%08lu").
 */
static int dmabuf_listed(unsigned long ino)
{
	char line[512], needle[32];
	FILE *f = fopen("/sys/kernel/debug/dma_buf/bufinfo", "r");
	int found = 0;

	if (!f)
		return -1;
	snprintf(needle, sizeof(needle), "\t%08lu\t", ino);
	while (fgets(line, sizeof(line), f))
		if (strstr(line, needle))
			found = 1;
	fclose(f);
	return found;
}

/*
 * Codec2 allocates the codec's linear buffers from /dev/dma_heap/system, and
 * libmemory_c2 asks /dev/ion for their M4U addresses as for ION buffers. As
 * on stock, the mapping lives as long as the buffer, not the handle.
 */
static void test_foreign_iova(void)
{
	struct dma_heap_allocation_data heap = {
		.len = BUF_LEN, .fd_flags = O_RDWR | O_CLOEXEC,
	};
	struct ion_fd_data imp;
	struct ion_handle_data hd;
	unsigned long addr = 0, again, len;
	struct ion_mm_data mm;
	int ion, heapfd, ret, listed;
	struct stat st;

	heapfd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	CHECK(heapfd >= 0, "open /dev/dma_heap/system");
	if (heapfd < 0)
		return;
	ret = xioctl(heapfd, DMA_HEAP_IOCTL_ALLOC, &heap);
	close(heapfd);
	CHECK(ret == 0, "DMA_HEAP_IOCTL_ALLOC %d bytes -> %d", BUF_LEN, ret);
	if (ret)
		return;
	fstat(heap.fd, &st);

	ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	CHECK(ion >= 0, "open /dev/ion");
	if (ion < 0)
		goto out_buf;

	memset(&imp, 0, sizeof(imp));
	imp.fd = heap.fd;
	ret = xioctl(ion, ION_IOC_IMPORT, &imp);
	CHECK(ret == 0 && imp.handle > 0, "ION_IOC_IMPORT of a DMA heap buffer -> %d", ret);
	if (ret)
		goto out_ion;

	ret = ion_phys(ion, imp.handle, &again, &len);
	CHECK(ret < 0, "no address before ION_MM_CONFIG_BUFFER (%d)", ret);

	CHECK(ion_config(ion, imp.handle, PORT_VENC_RCPU) == 0, "ION_MM_CONFIG_BUFFER VENC_RCPU");
	ret = ion_phys(ion, imp.handle, &addr, &len);
	CHECK(ret == 0 && addr && !(addr & 0xfff) && (uint64_t)addr + len <= 1ULL << 32 &&
	      len == BUF_LEN, "ION_SYS_GET_PHYS -> %d, IOVA %#lx + %#lx", ret, addr, len);
	ret = ion_phys(ion, imp.handle, &again, &len);
	CHECK(ret == 0 && again == addr, "same IOVA on the next query (%#lx)", again);

	memset(&mm, 0, sizeof(mm));
	mm.mm_cmd = ION_MM_GET_IOVA;
	mm.get_phys_param.handle = imp.handle;
	mm.get_phys_param.module_id = PORT_VENC_CUR_LUMA;
	ret = ion_custom(ion, ION_CMD_MULTIMEDIA, &mm);
	CHECK(ret == 0 && mm.get_phys_param.phy_addr == addr && mm.get_phys_param.len == BUF_LEN,
	      "ION_MM_GET_IOVA VENC_CUR_LUMA -> %d, the same mapping (%#llx)", ret,
	      (unsigned long long)mm.get_phys_param.phy_addr);

	/* The buffer outlives its handle: a new import finds port and mapping. */
	hd.handle = imp.handle;
	CHECK(xioctl(ion, ION_IOC_FREE, &hd) == 0, "ION_IOC_FREE");
	ret = xioctl(ion, ION_IOC_IMPORT, &imp);
	CHECK(ret == 0, "ION_IOC_IMPORT again -> %d", ret);
	if (!ret) {
		ret = ion_phys(ion, imp.handle, &again, &len);
		CHECK(ret == 0 && again == addr, "same IOVA without a new CONFIG_BUFFER (%#lx)", again);
		hd.handle = imp.handle;
		xioctl(ion, ION_IOC_FREE, &hd);
	}
out_ion:
	close(ion);
out_buf:
	close(heap.fd);

	/* Released once nothing but the mapping holds it (swept every second). */
	usleep(2500 * 1000);
	listed = dmabuf_listed(st.st_ino);
	if (listed < 0)
		printf("SKIP buffer released after its last fd (no debugfs)\n");
	else
		CHECK(!listed, "buffer released after its last fd");
}

int main(void)
{
	int a = open("/dev/Vcodec", O_RDWR | O_CLOEXEC);
	int b = open("/dev/Vcodec", O_RDWR | O_CLOEXEC);

	printf("r1-vcodec-test (%zu-bit)\n", sizeof(void *) * 8);
	CHECK(a >= 0 && b >= 0, "open /dev/Vcodec twice");
	if (a < 0 || b < 0)
		return 1;

	test_misc(a, b);
	test_mmap(a);
	test_lock(a, b);
	test_contention(a, VAL_DRIVER_TYPE_H264_DEC);
	test_contention(a, VAL_DRIVER_TYPE_H264_ENC);
	test_abandon(a);
	test_iova();
	test_foreign_iova();

	close(b);
	close(a);
	printf("%d failure(s)\n", failures);
	return failures;
}
