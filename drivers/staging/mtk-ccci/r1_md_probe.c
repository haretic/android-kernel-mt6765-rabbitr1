// SPDX-License-Identifier: GPL-2.0
/*
 * Rabbit r1 (MT6765) modem bring-up probe.
 *
 * Exercises the boot path of the integrated MD1 (gen93) modem the way the
 * stock 4.19 CCCI driver does (eccci/mt6765/md_sys1_platform.c and
 * hif/ccci_hif_ccif.c), one stage at a time, without the CCCI stack:
 *
 *   stage=0  read only: decode the modem handoff LK leaves in
 *            /chosen/ccci,modem_info_v2 and print the MD1 power, bus
 *            protection and clock state. Nothing is written.
 *   stage=1  also power MD1 (topckgen MD clock mode, VMODEM, MTCMOS with
 *            bus protection, srcclkena, MD PLLs, MD WDT off). The modem
 *            core stays parked: its boot vector is not enabled.
 *   stage=2  also prepare the CCIF hardware and the shared memory ring
 *            buffers, release the modem and wait for its first handshake
 *            (HS1, MD_INIT_START_BOOT on the control channel).
 *
 * The modem image is not loaded here: LK has loaded, verified and
 * protected it before Linux starts. Every wait is bounded. Nothing but SoC
 * registers and the AP/MD shared memory is written; no storage is touched.
 * Removing the module powers MD1 down again.
 */

#define pr_fmt(fmt) "r1-md-probe: " fmt

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

static unsigned int stage;
module_param(stage, uint, 0444);
MODULE_PARM_DESC(stage, "0: read only (default), 1: power MD1, 2: also release the modem and wait for HS1");

static unsigned int hs1_timeout_ms = 10000;
module_param(hs1_timeout_ms, uint, 0444);
MODULE_PARM_DESC(hs1_timeout_ms, "How long stage 2 waits for HS1 (ms, max 60000)");

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "Stage 2 polling interval (ms, 1-1000)");

static bool clear_smem = true;
module_param(clear_smem, bool, 0444);
MODULE_PARM_DESC(clear_smem, "Clear the shared memory regions stock clears before the first modem boot");

static bool keep_on_fail;
module_param(keep_on_fail, bool, 0444);
MODULE_PARM_DESC(keep_on_fail, "Leave the modem running when no HS1 arrives (default: power it down and fail the load)");

/* SPM (scpsys syscon) */
#define SPM_PWR_STATUS			0x180
#define SPM_PWR_STATUS_2ND		0x184
#define SPM_MD1_PWR_CON			0x320
#define SPM_MD_SRAM_ISO_CON		0x394
#define SPM_MD_EXTRA_PWR_CON		0x398
#define SPM_MD1_PWR_STA			BIT(0)

/* INFRACFG_AO (infracfg syscon) */
#define INFRA_RST0			0x150
#define INFRA_RST1			0x154
#define INFRA_CCIF0_RST			BIT(8)
#define INFRA_TOPAXI_PROTECTEN		0x220
#define INFRA_TOPAXI_PROTECTEN_STA1	0x228
#define INFRA_TOPAXI_PROTECTEN_1	0x250
#define INFRA_TOPAXI_PROTECTEN_STA1_1	0x258
#define INFRA_TOPAXI_PROTECTEN_SET	0x2a0
#define INFRA_TOPAXI_PROTECTEN_1_SET	0x2a8
#define INFRA_TOPAXI_PROTECTEN_1_CLR	0x2ac
#define INFRA_PERI2MD_PROT		BIT(7)	/* in PROTECTEN */
#define INFRA_MD2PERI_PROT		BIT(6)	/* in PROTECTEN_1 */
#define INFRA_AP2MD_DUMMY		0x370
#define INFRA_AP2MD_DUMMY_BIT		BIT(0)
#define INFRA_MD_SRCCLKENA		0xf0c

/* TOPCKGEN CLK_MODE: MD clock gating, cleared before MD power on */
#define TOPCKGEN_CLK_MODE		0x0
#define TOPCKGEN_MD_CLK_GATE		(BIT(8) | BIT(9))

/* APMIXEDSYS */
#define APMIXED_AP_PLL_CON0		0x0

/* MD side registers, AP view; only reachable while PERI2MD is open */
#define MD_BOOT_VECTOR_EN		0x20000024
#define MD_RGU_BASE			0x200f0100
#define MD_WDT_MODE			0x0
#define MD_WDT_MODE_KEY			0x55000030
#define MDTOP_PLLMIXED_BASE		0x20140000
#define MDTOP_CLKSW_BASE		0x20150000
#define MDTOP_LEN			0x1000

/* CCIF */
#define APCCIF_CON			0x00
#define APCCIF_BUSY			0x04
#define APCCIF_START			0x08
#define APCCIF_TCHNUM			0x0c
#define APCCIF_RCHNUM			0x10
#define APCCIF_ACK			0x14
#define APCCIF_CHDATA			0x100
#define CCIF_SRAM_SIZE			512
#define CCIF_BOOT_TRACE_LEN		60
#define CCIF_RX_EXCEPTION_INIT		16

/* LK handoff */
#define LK_INFO_MAX_SIZE		0x10000
#define LK_TAG_NAME_LEN_V1		16
#define LK_TAG_NAME_LEN_V2		64

/*
 * Shared memory, gen93 layout (eccci/ccci_modem.c md1_6293_noncacheable_fat).
 * The MD sees the non-cacheable region at its bank 4, 0x40000000.
 */
#define SMEM_MD_VIEW_BASE		0x40000000
#define SMEM_MDSS_DBG_OFF		(2 * 1024)
#define SMEM_MDSS_DBG_SIZE		(10 * 1024)
#define SMEM_CCB_CTRL_OFF		(96 * 1024)
#define SMEM_NETD_OFF			(100 * 1024)
#define SMEM_AUDIO_OFF			(108 * 1024)
#define SMEM_CCISM_MCU_OFF		(160 * 1024)
#define SMEM_CCISM_MCU_SIZE		((720 + 1) * 1024)
#define SMEM_CCISM_MCU_EXP_OFF		(881 * 1024)
#define SMEM_CCISM_MCU_EXP_SIZE		((120 + 1) * 1024)
#define SMEM_DFD_OFF			(1024 * 1024)

/* CCIF ring buffers, eccci/hif/ccci_ringbuf.c */
#define CCIF_QUEUE_NUM			8
#define RBF_HEADER			0xee0000ee
#define RBF_FOOTER			0xff0000ff
#define RBF_PKG_HEADER			0xaabbaabb
#define RBF_PKG_FOOTER			0xccddeeff
#define RBF_PKG_HDR_LEN			8
#define RBF_PKG_FTR_LEN			8
/* 8 byte header, struct ccci_ringbuf (two {read, write, length}), footer */
#define RBF_CTL_LEN			(8 + 24 + 8)
#define RBF_RX_READ			0x8
#define RBF_RX_WRITE			0xc
#define RBF_RX_LENGTH			0x10
#define RBF_TX_READ			0x14
#define RBF_TX_WRITE			0x18
#define RBF_TX_LENGTH			0x1c
#define RBF_BUFFER			0x20

static const u32 rbf_rx_size[CCIF_QUEUE_NUM] = {
	80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
};

static const u32 rbf_tx_size[CCIF_QUEUE_NUM] = {
	128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024,
	20 * 1024, 20 * 1024, 64 * 1024, 0,
};

static const u32 rbf_exp_rx_size[CCIF_QUEUE_NUM] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0,
};

static const u32 rbf_exp_tx_size[CCIF_QUEUE_NUM] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0,
};

/* struct ccci_header and the HS1 markers, mt-plat/mtk_ccci_common.h */
#define CCCI_HEADER_LEN			16
#define CCCI_CONTROL_RX			0
#define MD_INIT_START_BOOT		0x0
#define MD_INIT_CHK_ID			0x5555ffff

static const char * const r1md_clk_names[] = {
	"cldma-bclk", "ccif-ap", "ccif-md", "ccif1-ap", "ccif1-md",
	"ccif2-ap", "ccif2-md",
};

struct r1md_handoff {
	bool valid;
	phys_addr_t md_base;
	u32 md_size;
	phys_addr_t nc_smem_base;
	u32 nc_smem_size;
	phys_addr_t c_smem_base;
	u32 c_smem_size;
	s32 dfd_size;
};

struct r1md_map {
	void *va;
	size_t size;
	bool vmapped;
};

struct r1md {
	struct device *dev;
	struct regmap *spm;
	struct regmap *infracfg;
	struct regmap *topckgen;
	struct regmap *apmixed;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	void __iomem *boot_sta;
	struct clk_bulk_data clks[ARRAY_SIZE(r1md_clk_names)];
	struct regulator *vmodem;
	struct regulator *vcore;
	struct r1md_map nc_smem;
	struct r1md_map c_smem;
	bool clks_on;
	bool vmodem_on;
	bool pd_on;
	bool srcclkena_set;
	bool released;
	bool hs1;
};

static struct r1md_handoff handoff;
/* outcome of the probe, reported back to insmod; 1 until it runs */
static int r1md_result = 1;

static void *r1md_map_mem(struct r1md_map *map, phys_addr_t phys, size_t size)
{
	unsigned long pfn = PHYS_PFN(phys);
	unsigned long n = PFN_UP(size);
	struct page **pages;
	unsigned long i;

	map->size = size;
	map->vmapped = false;
	map->va = NULL;

	if (!PAGE_ALIGNED(phys) || !size)
		return NULL;

	for (i = 0; i < n; i++)
		if (!pfn_valid(pfn + i))
			break;

	if (i < n) {
		/* not covered by the memmap: plain write-combined I/O mapping */
		map->va = memremap(phys, size, MEMREMAP_WC);
		return map->va;
	}

	/*
	 * Kernel-visible memory: the region is a reserved, never touched part
	 * of the linear map, so an uncached alias of its pages is what the
	 * vendor driver (vmap_reserved_mem) and DMA remapping do as well.
	 */
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(pfn + i);
	map->va = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kvfree(pages);
	map->vmapped = !!map->va;

	return map->va;
}

static void r1md_unmap_mem(struct r1md_map *map)
{
	if (!map->va)
		return;
	if (map->vmapped)
		vunmap(map->va);
	else
		memunmap(map->va);
	map->va = NULL;
}

static u32 r1md_rd(struct regmap *map, unsigned int reg)
{
	unsigned int val = 0;

	if (regmap_read(map, reg, &val))
		return 0xdeadbeef;
	return val;
}

/* ------------------------------------------------------------------------
 * Stage 0: LK handoff and status, read only
 */

static void r1md_decode_tag(const char *name, const u8 *d, u32 size)
{
	u32 i, n;

	if (!strcmp(name, "hdr_count") && size >= 4) {
		pr_info("  hdr_count: %u\n", get_unaligned_le32(d));
	} else if (!strcmp(name, "hdr_tbl_inf")) {
		/* struct _modem_info: u64 base, u32 size, s8 md_id, errno, type, ver, u32 rsv[2] */
		for (i = 0; i + 24 <= size; i += 24) {
			u64 base = get_unaligned_le64(d + i);
			u32 sz = get_unaligned_le32(d + i + 8);
			u8 md_id = d[i + 12];
			s8 err = d[i + 13];

			if (!base && !sz)
				continue;
			pr_info("  hdr_tbl_inf[%u]: md_id %u base %#llx size %#x errno %d type %u ver %u\n",
				i / 24, md_id, base, sz, err, d[i + 14], d[i + 15]);
			if (md_id == 0 && !err) {
				handoff.md_base = base;
				handoff.md_size = sz;
			}
		}
	} else if (!strcmp(name, "smem_layout") && size >= 36) {
		u64 base = get_unaligned_le64(d);
		u32 off = get_unaligned_le32(d + 8);
		u32 sz = get_unaligned_le32(d + 12);

		pr_info("  smem_layout: base %#llx ap_md1 +%#x size %#x, ap_md3 +%#x size %#x, md1_md3 +%#x size %#x, total %#x\n",
			base, off, sz, get_unaligned_le32(d + 16),
			get_unaligned_le32(d + 20), get_unaligned_le32(d + 24),
			get_unaligned_le32(d + 28), get_unaligned_le32(d + 32));
		handoff.nc_smem_base = base + off;
		handoff.nc_smem_size = sz;
	} else if (!strcmp(name, "ccb_info") && size >= 12) {
		handoff.c_smem_base = get_unaligned_le64(d);
		handoff.c_smem_size = get_unaligned_le32(d + 8);
		pr_info("  ccb_info: data %#llx size %#x\n",
			(u64)handoff.c_smem_base, handoff.c_smem_size);
	} else if (!strcmp(name, "md_mpu_inf")) {
		/* struct _mpu_cfg: u32 start, end; s32 region; u32 perm; s32 relate */
		for (i = 0; i + 20 <= size; i += 20)
			pr_info("  md_mpu_inf: region %d %#010x-%#010x permission %#x relate %d\n",
				(s32)get_unaligned_le32(d + i + 8),
				get_unaligned_le32(d + i),
				get_unaligned_le32(d + i + 4),
				get_unaligned_le32(d + i + 12),
				(s32)get_unaligned_le32(d + i + 16));
	} else if (!strcmp(name, "md1_chk") && size >= 192) {
		/* struct md_check_header_v5/v6 common prefix */
		pr_info("  md1_chk: magic %.12s ver %u product %u type %u platform %.16s\n",
			d, get_unaligned_le32(d + 12), get_unaligned_le32(d + 16),
			get_unaligned_le32(d + 20), d + 24);
		pr_info("  md1_chk: build %.64s (%.64s)\n", d + 104, d + 40);
		pr_info("  md1_chk: mem_size %#x img_size %#x dsp +%#x size %#x\n",
			get_unaligned_le32(d + 172), get_unaligned_le32(d + 176),
			get_unaligned_le32(d + 184), get_unaligned_le32(d + 188));
	} else if (!strcmp(name, "nc_smem_info_ext")) {
		/* struct nc_smem_node: ap_offset, md_offset, size, id */
		for (i = 0; i + 16 <= size; i += 16)
			pr_info("  nc_smem id %u: ap +%#x md +%#x size %#x\n",
				get_unaligned_le32(d + i + 12),
				get_unaligned_le32(d + i),
				get_unaligned_le32(d + i + 4),
				get_unaligned_le32(d + i + 8));
	} else if (!strncmp(name, "retrieve", 8) && strcmp(name, "retrieve_num") &&
		   size >= 16) {
		pr_info("  %s: %#llx size %#llx\n", name,
			get_unaligned_le64(d), get_unaligned_le64(d + 8));
	} else if (size == 4) {
		n = get_unaligned_le32(d);
		pr_info("  %s: %u (%#x)\n", name, n, n);
		if (!strcmp(name, "smem_dfd_size"))
			handoff.dfd_size = n;
	} else {
		pr_info("  %s: %u bytes\n", name, size);
	}
}

static int r1md_parse_tags(phys_addr_t base, u32 size, u32 version, u32 count)
{
	u32 name_len = version >= 2 ? LK_TAG_NAME_LEN_V2 : LK_TAG_NAME_LEN_V1;
	struct r1md_map map;
	char name[LK_TAG_NAME_LEN_V2 + 1];
	u8 *blob, *buf;
	u32 off = 0, i;

	if (!size || size > LK_INFO_MAX_SIZE) {
		pr_err("handoff: tag area size %#x out of range\n", size);
		return -EINVAL;
	}

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	blob = r1md_map_mem(&map, base, PAGE_ALIGN(size));
	if (!blob) {
		pr_err("handoff: cannot map tag area at %pa\n", &base);
		kfree(buf);
		return -ENOMEM;
	}
	/* copy once; the area must stay intact for the CCCI driver */
	memcpy(buf, blob, size);
	r1md_unmap_mem(&map);

	for (i = 0; i < count; i++) {
		u32 data_off, data_size, next;

		if (off + name_len + 12 > size) {
			pr_err("handoff: tag %u header at %#x beyond the area\n", i, off);
			break;
		}
		memcpy(name, buf + off, name_len);
		name[name_len] = '\0';
		data_off = get_unaligned_le32(buf + off + name_len);
		data_size = get_unaligned_le32(buf + off + name_len + 4);
		next = get_unaligned_le32(buf + off + name_len + 8);

		if (data_off > size || data_size > size - data_off) {
			pr_err("handoff: tag %s data %#x+%#x beyond the area\n",
			       name, data_off, data_size);
			break;
		}
		r1md_decode_tag(name, buf + data_off, data_size);

		if (next <= off && i + 1 < count) {
			pr_err("handoff: tag %s next offset %#x does not advance\n",
			       name, next);
			break;
		}
		off = next;
	}

	kfree(buf);
	return 0;
}

static int r1md_read_handoff(void)
{
	struct device_node *chosen, *rmem, *child;
	const u8 *prop;
	int len, ret, i;
	u64 base;
	u32 size, version, count;
	s32 err;

	handoff.dfd_size = -1;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen) {
		pr_err("handoff: no /chosen\n");
		return -ENODEV;
	}

	prop = of_get_property(chosen, "atag,mdinfo", &len);
	pr_info("handoff: /chosen/atag,mdinfo %d bytes\n", prop ? len : -1);
	prop = of_get_property(chosen, "atag,boot", &len);
	pr_info("handoff: /chosen/atag,boot %d bytes\n", prop ? len : -1);

	prop = of_get_property(chosen, "ccci,modem_info_v2", &len);
	if (!prop || len < 28) {
		pr_err("handoff: /chosen/ccci,modem_info_v2 missing or short (%d)\n",
		       prop ? len : -1);
		of_node_put(chosen);
		return -ENODEV;
	}

	/* struct _ccci_lk_info_v2, little endian as LK copied it */
	base = get_unaligned_le64(prop);
	size = get_unaligned_le32(prop + 8);
	err = get_unaligned_le32(prop + 12);
	version = get_unaligned_le32(prop + 16);
	count = get_unaligned_le32(prop + 20);
	pr_info("handoff: modem_info_v2 %d bytes: tags at %#llx size %#x, version %u, %u tags, LK errno %d, load flag %#x\n",
		len, base, size, version, count, err,
		get_unaligned_le32(prop + 24));
	for (i = 0; 28 + 4 * (i + 1) <= len && i < 4; i++)
		pr_info("handoff: md%d LK load errno %d\n", i + 1,
			(s32)get_unaligned_le32(prop + 28 + 4 * i));
	of_node_put(chosen);

	if (!base) {
		pr_err("handoff: LK loaded no modem\n");
		return -ENODEV;
	}

	ret = r1md_parse_tags(base, size, version, count);
	if (ret)
		return ret;

	rmem = of_find_node_by_path("/reserved-memory");
	for_each_child_of_node(rmem, child) {
		struct resource res;

		if (!strstr(child->name, "ccci") && !strstr(child->name, "md_"))
			continue;
		if (!of_address_to_resource(child, 0, &res))
			pr_info("reserved-memory %s: %pR%s\n", child->name, &res,
				of_property_present(child, "no-map") ? " no-map" : "");
	}
	of_node_put(rmem);

	handoff.valid = handoff.md_base && handoff.md_size &&
			handoff.nc_smem_base && handoff.nc_smem_size;
	pr_info("handoff: md %#llx+%#x, nc smem %#llx+%#x, c smem %#llx+%#x, dfd %d: %s\n",
		(u64)handoff.md_base, handoff.md_size,
		(u64)handoff.nc_smem_base, handoff.nc_smem_size,
		(u64)handoff.c_smem_base, handoff.c_smem_size, handoff.dfd_size,
		handoff.valid ? "usable" : "INCOMPLETE");

	return handoff.valid ? 0 : -ENODEV;
}

static void r1md_dump_status(const char *when, struct regmap *spm,
			     struct regmap *infracfg, struct regmap *topckgen)
{
	if (spm)
		pr_info("%s: SPM PWR_STATUS %#x/%#x (MD1 %s) MD1_PWR_CON %#x MD_SRAM_ISO_CON %#x MD_EXTRA_PWR_CON %#x\n",
			when, r1md_rd(spm, SPM_PWR_STATUS),
			r1md_rd(spm, SPM_PWR_STATUS_2ND),
			(r1md_rd(spm, SPM_PWR_STATUS) & SPM_MD1_PWR_STA) &&
			(r1md_rd(spm, SPM_PWR_STATUS_2ND) & SPM_MD1_PWR_STA) ?
			"on" : "off",
			r1md_rd(spm, SPM_MD1_PWR_CON),
			r1md_rd(spm, SPM_MD_SRAM_ISO_CON),
			r1md_rd(spm, SPM_MD_EXTRA_PWR_CON));
	if (infracfg)
		pr_info("%s: INFRA PROTECTEN %#x STA1 %#x PROTECTEN_1 %#x STA1_1 %#x AP2MD %#x SRCCLKENA %#x\n",
			when, r1md_rd(infracfg, INFRA_TOPAXI_PROTECTEN),
			r1md_rd(infracfg, INFRA_TOPAXI_PROTECTEN_STA1),
			r1md_rd(infracfg, INFRA_TOPAXI_PROTECTEN_1),
			r1md_rd(infracfg, INFRA_TOPAXI_PROTECTEN_STA1_1),
			r1md_rd(infracfg, INFRA_AP2MD_DUMMY),
			r1md_rd(infracfg, INFRA_MD_SRCCLKENA));
	if (topckgen)
		pr_info("%s: TOPCKGEN CLK_MODE %#x\n", when,
			r1md_rd(topckgen, TOPCKGEN_CLK_MODE));
}

static bool r1md_md1_is_on(struct regmap *spm)
{
	return (r1md_rd(spm, SPM_PWR_STATUS) & SPM_MD1_PWR_STA) &&
	       (r1md_rd(spm, SPM_PWR_STATUS_2ND) & SPM_MD1_PWR_STA);
}

/* ------------------------------------------------------------------------
 * Stage 1: power on, md_cd_power_on() without the image load
 */

static int r1md_poll_infra(struct r1md *md, unsigned int reg, u32 mask, u32 want)
{
	unsigned int val;
	int ret;

	ret = regmap_read_poll_timeout(md->infracfg, reg, val,
				       (val & mask) == want, 10, 100000);
	if (ret)
		pr_err("infracfg %#x: %#x, wanted %#x under %#x, timed out\n",
		       reg, val, want, mask);
	return ret;
}

/* md1_pre_access_md_reg(): stop MD->AP traffic while AP touches MD */
static int r1md_pre_access(struct r1md *md)
{
	regmap_clear_bits(md->infracfg, INFRA_AP2MD_DUMMY, INFRA_AP2MD_DUMMY_BIT);
	regmap_write(md->infracfg, INFRA_TOPAXI_PROTECTEN_1_SET, INFRA_MD2PERI_PROT);
	return r1md_poll_infra(md, INFRA_TOPAXI_PROTECTEN_STA1_1,
			       INFRA_MD2PERI_PROT, INFRA_MD2PERI_PROT);
}

/* md1_post_access_md_reg(): close AP->MD, open MD->AP, flag MD may access AP */
static int r1md_post_access(struct r1md *md)
{
	int ret;

	regmap_write(md->infracfg, INFRA_TOPAXI_PROTECTEN_SET, INFRA_PERI2MD_PROT);
	ret = r1md_poll_infra(md, INFRA_TOPAXI_PROTECTEN_STA1,
			      INFRA_PERI2MD_PROT, INFRA_PERI2MD_PROT);
	if (ret)
		return ret;

	regmap_write(md->infracfg, INFRA_TOPAXI_PROTECTEN_1_CLR, INFRA_MD2PERI_PROT);
	ret = r1md_poll_infra(md, INFRA_TOPAXI_PROTECTEN_STA1_1,
			      INFRA_MD2PERI_PROT, 0);
	if (ret)
		return ret;

	regmap_set_bits(md->infracfg, INFRA_AP2MD_DUMMY, INFRA_AP2MD_DUMMY_BIT);
	pr_info("post access: PROTECTEN %#x PROTECTEN_1 %#x AP2MD %#x\n",
		r1md_rd(md->infracfg, INFRA_TOPAXI_PROTECTEN),
		r1md_rd(md->infracfg, INFRA_TOPAXI_PROTECTEN_1),
		r1md_rd(md->infracfg, INFRA_AP2MD_DUMMY));
	return 0;
}

/*
 * Refuse to touch MD side registers unless MD1 is powered and the AP->MD
 * path is open: an access into a protected or unpowered MD hangs the bus.
 */
static int r1md_md_reachable(struct r1md *md)
{
	u32 sta1 = r1md_rd(md->infracfg, INFRA_TOPAXI_PROTECTEN_STA1);

	if (!r1md_md1_is_on(md->spm)) {
		pr_err("MD1 is not powered, not touching MD registers\n");
		return -EIO;
	}
	if (sta1 & INFRA_PERI2MD_PROT) {
		pr_err("PERI2MD protection is active (STA1 %#x), not touching MD registers\n",
		       sta1);
		return -EIO;
	}
	return 0;
}

/* md1_pll_init(), with every loop bounded */
static int r1md_pll_init(struct r1md *md)
{
	void __iomem *pll, *clksw;
	u32 val;
	int ret, i;

	pll = ioremap(MDTOP_PLLMIXED_BASE, MDTOP_LEN);
	clksw = ioremap(MDTOP_CLKSW_BASE, MDTOP_LEN);
	if (!pll || !clksw) {
		ret = -ENOMEM;
		goto out;
	}

	ret = readl_poll_timeout(pll, val, val, 20000, 1000000);
	pr_info("pll: version %#x\n", val);
	if (ret) {
		pr_err("pll: MD PLL block reads 0 after 1 s; MD clock/power path is not up\n");
		goto out;
	}

	/* clock square1 low-pass filter for 26 MHz quality */
	regmap_set_bits(md->apmixed, APMIXED_AP_PLL_CON0, BIT(1));
	usleep_range(100, 200);

	/* md_srclkena_ack settle time: 136T of 32 kHz */
	writel(0x02020e88, pll + 0x4);

	writel(0x801713b1, pll + 0x60);
	writel(0x80171400, pll + 0x58);
	writel(0x80229e00, pll + 0x50);
	writel(0x80204e00, pll + 0x48);
	writel(0x80213c00, pll + 0x40);

	ret = readl_poll_timeout(pll + 0xc00, val, !(val & BIT(14)), 10, 100000);
	if (ret) {
		pr_err("pll: +0xc00 bit 14 stays set (%#x)\n", val);
		goto out;
	}

	writel(readl(pll + 0x64) & ~0x80, pll + 0x64);

	/* the stock build has no CONFIG_ARCH_MT6765, so +0x104 is left alone */
	writel(0x100010, pll + 0x10);
	for (i = 1; i <= 50; i++) {
		val = readl(pll + 0x10);
		if (val == 0x100010)
			break;
		if (!(i % 5)) {
			pr_info("pll: rewrite 0x100010 (%d)\n", i);
			writel(0x100010, pll + 0x10);
		}
		msleep(20);
	}
	pr_info("pll: +0x10 %#x after %d tries\n", val, i);
	if (val != 0x100010) {
		ret = -ETIMEDOUT;
		goto out;
	}

	for (i = 0; i < 50; i++) {
		val = readl(clksw + 0x84);
		if (val & BIT(15))
			break;
		msleep(20);
	}
	pr_info("pll: clksw +0x84 %#x\n", val);
	if (!(val & BIT(15))) {
		pr_err("pll: MD clock switch never reported ready\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	writel(readl(clksw + 0x24) | 0x3, clksw + 0x24);
	writel(readl(clksw + 0x24) | 0x58103fc, clksw + 0x24);
	writel(readl(clksw + 0x28) | 0x10, clksw + 0x28);
	writel(0x1, clksw + 0x20);

	writel(0xffff, pll + 0x314);
	writel(0xffff, pll + 0x318);

	/* record that the MD PLLs are initialised */
	writel(0x62930000, pll + 0xf00);
	pr_info("pll: init done\n");
	ret = 0;
out:
	if (clksw)
		iounmap(clksw);
	if (pll)
		iounmap(pll);
	return ret;
}

static int r1md_md_wdt_off(void)
{
	void __iomem *rgu = ioremap(MD_RGU_BASE, 0x300);

	if (!rgu)
		return -ENOMEM;
	writel(MD_WDT_MODE_KEY, rgu + MD_WDT_MODE);
	pr_info("MD WDT mode %#x\n", readl(rgu + MD_WDT_MODE));
	iounmap(rgu);
	return 0;
}

static int r1md_power_on(struct r1md *md)
{
	int ret, uv;

	/* step 2: VMODEM 0.8 V, kept on by its always-on constraint */
	ret = regulator_enable(md->vmodem);
	if (ret) {
		pr_err("vmodem enable: %d\n", ret);
		return ret;
	}
	md->vmodem_on = true;
	ret = regulator_set_voltage(md->vmodem, 800000, 800000);
	if (!ret)
		ret = regulator_sync_voltage(md->vmodem);
	uv = regulator_get_voltage(md->vmodem);
	pr_info("vmodem %d uV (set: %d)\n", uv, ret);
	if (ret)
		return ret;
	if (md->vcore)
		pr_info("vcore %d uV\n", regulator_get_voltage(md->vcore));

	/*
	 * step 3: MD1 MTCMOS, with the stock bus protection sequence in the
	 * domain. The driver core already powered it when it attached the
	 * device (VMODEM is pinned to 0.8 V from boot); hold it on from here.
	 */
	ret = pm_runtime_resume_and_get(md->dev);
	if (ret) {
		pr_err("MD1 power domain on: %d\n", ret);
		return ret;
	}
	md->pd_on = true;
	r1md_dump_status("MD1 on", md->spm, md->infracfg, md->topckgen);
	if (!r1md_md1_is_on(md->spm)) {
		pr_err("MD1 power domain reports on but SPM status is off\n");
		return -EIO;
	}

	ret = r1md_md_reachable(md);
	if (ret)
		return ret;

	ret = r1md_pre_access(md);
	if (ret)
		return ret;

	/* step 4: MD srcclkena */
	regmap_update_bits(md->infracfg, INFRA_MD_SRCCLKENA, 0xff, 0x21);
	md->srcclkena_set = true;
	pr_info("srcclkena %#x\n", r1md_rd(md->infracfg, INFRA_MD_SRCCLKENA));

	/* step 5: MD PLLs */
	ret = r1md_pll_init(md);
	if (ret)
		return ret;

	/* step 6: MD watchdog off */
	return r1md_md_wdt_off();
}

/* ------------------------------------------------------------------------
 * Stage 2: CCIF, shared memory, release, HS1
 */

static void r1md_ccif_reset(struct r1md *md)
{
	u32 __iomem *tail;
	unsigned int i;

	/* clears CCIF busy/channel/irq state, not the SRAM */
	regmap_write(md->infracfg, INFRA_RST0, INFRA_CCIF0_RST);
	regmap_write(md->infracfg, INFRA_RST1, INFRA_CCIF0_RST);

	for (i = 0; i < CCIF_SRAM_SIZE; i += 4) {
		writel(0, md->ap_ccif + APCCIF_CHDATA + i);
		writel(0, md->md_ccif + APCCIF_CHDATA + i);
	}

	/* last 12 bytes: magic, MD view of MDSS_DBG and its size */
	tail = md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_SIZE - 12;
	writel(0x7274626e, tail);
	writel(SMEM_MD_VIEW_BASE + SMEM_MDSS_DBG_OFF, tail + 1);
	writel(SMEM_MDSS_DBG_SIZE, tail + 2);
}

static void r1md_smem_zero(struct r1md *md, u32 off, u32 len)
{
	if (off >= md->nc_smem.size)
		return;
	len = min_t(u32, len, md->nc_smem.size - off);
	memset(md->nc_smem.va + off, 0, len);
}

/* ccci_create_ringbuf() for each queue, laid out back to back */
static int r1md_ringbufs_init(struct r1md *md, u32 off, u32 room,
			      const u32 *rx, const u32 *tx, const char *what)
{
	u8 *p = md->nc_smem.va + off;
	u32 used = 0, len;
	int q;

	memset(p, 0, room);
	for (q = 0; q < CCIF_QUEUE_NUM; q++) {
		len = RBF_CTL_LEN + rx[q] + tx[q];
		if (used + len > room) {
			pr_err("%s ring buffers overflow their %#x bytes\n", what, room);
			return -ENOSPC;
		}
		WRITE_ONCE(*(u32 *)(p + 0), RBF_HEADER);
		WRITE_ONCE(*(u32 *)(p + 4), RBF_HEADER);
		WRITE_ONCE(*(u32 *)(p + RBF_RX_LENGTH), rx[q]);
		WRITE_ONCE(*(u32 *)(p + RBF_TX_LENGTH), tx[q]);
		WRITE_ONCE(*(u32 *)(p + len - 8), RBF_FOOTER);
		WRITE_ONCE(*(u32 *)(p + len - 4), RBF_FOOTER);
		p += len;
		used += len;
	}
	pr_info("%s ring buffers: %u queues in %#x of %#x bytes at smem +%#x\n",
		what, CCIF_QUEUE_NUM, used, room, off);
	return 0;
}

static int r1md_smem_prepare(struct r1md *md)
{
	int ret;

	if (!handoff.valid)
		return -ENODEV;

	if (handoff.nc_smem_size < SMEM_CCISM_MCU_EXP_OFF + SMEM_CCISM_MCU_EXP_SIZE) {
		pr_err("nc smem %#x is smaller than the gen93 layout\n",
		       handoff.nc_smem_size);
		return -EINVAL;
	}

	if (!r1md_map_mem(&md->nc_smem, handoff.nc_smem_base, handoff.nc_smem_size)) {
		pr_err("cannot map nc smem\n");
		return -ENOMEM;
	}
	pr_info("nc smem %#llx+%#x mapped (%s)\n", (u64)handoff.nc_smem_base,
		handoff.nc_smem_size, md->nc_smem.vmapped ? "vmap wc" : "memremap wc");

	if (clear_smem) {
		/* first boot: everything without SMF_NCLR_FIRST */
		r1md_smem_zero(md, 0, SMEM_CCB_CTRL_OFF);
		r1md_smem_zero(md, SMEM_NETD_OFF, SMEM_AUDIO_OFF - SMEM_NETD_OFF);
		r1md_smem_zero(md, SMEM_DFD_OFF, handoff.dfd_size > 0 ?
			       handoff.dfd_size : 0);

		if (handoff.c_smem_base && handoff.c_smem_size &&
		    r1md_map_mem(&md->c_smem, handoff.c_smem_base,
				 handoff.c_smem_size)) {
			memset(md->c_smem.va, 0, handoff.c_smem_size);
			pr_info("c smem %#llx+%#x cleared\n",
				(u64)handoff.c_smem_base, handoff.c_smem_size);
			r1md_unmap_mem(&md->c_smem);
		} else {
			pr_warn("c smem not cleared\n");
		}
	}

	ret = r1md_ringbufs_init(md, SMEM_CCISM_MCU_OFF, SMEM_CCISM_MCU_SIZE,
				 rbf_rx_size, rbf_tx_size, "normal");
	if (ret)
		return ret;
	ret = r1md_ringbufs_init(md, SMEM_CCISM_MCU_EXP_OFF, SMEM_CCISM_MCU_EXP_SIZE,
				 rbf_exp_rx_size, rbf_exp_tx_size, "exception");
	if (ret)
		return ret;

	/* the MD reads all of this through its own, uncached view */
	wmb();
	return 0;
}

static int r1md_let_md_go(struct r1md *md)
{
	void __iomem *vec;
	int ret;

	ret = r1md_md_reachable(md);
	if (ret)
		return ret;

	vec = ioremap(MD_BOOT_VECTOR_EN, 4);
	if (!vec)
		return -ENOMEM;
	writel(1, vec);
	pr_info("MD boot vector enable %#x: modem released\n", readl(vec));
	iounmap(vec);
	md->released = true;

	return r1md_post_access(md);
}

static u32 r1md_boot_sta(struct r1md *md, unsigned int idx)
{
	/* the stock driver keeps the third of three reads */
	readl(md->boot_sta + 4 * idx);
	readl(md->boot_sta + 4 * idx);
	return readl(md->boot_sta + 4 * idx);
}

/* one packet from queue 0's AP RX ring, eccci ccci_ringbuf_readable/read */
static void r1md_check_rx_q0(struct r1md *md, unsigned int elapsed)
{
	u8 *rb = md->nc_smem.va + SMEM_CCISM_MCU_OFF;
	u8 *buf = rb + RBF_BUFFER;
	u32 rd = READ_ONCE(*(u32 *)(rb + RBF_RX_READ));
	u32 wr = READ_ONCE(*(u32 *)(rb + RBF_RX_WRITE));
	u32 len = READ_ONCE(*(u32 *)(rb + RBF_RX_LENGTH));
	u32 hdr[2], hd[4], i, avail, pkg, pos, words;

	if (rd == wr)
		return;
	/* the write pointer is published after the data */
	rmb();

	if (len != rbf_rx_size[0] || rd >= len || wr >= len) {
		pr_err("q0 rx ring damaged: read %#x write %#x length %#x\n",
		       rd, wr, len);
		WRITE_ONCE(*(u32 *)(rb + RBF_RX_READ), wr);
		return;
	}

	avail = wr >= rd ? wr - rd : len - rd + wr;
	for (i = 0; i < 2; i++)
		hdr[i] = READ_ONCE(*(u32 *)(buf + (rd + 4 * i) % len));
	pkg = ALIGN(RBF_PKG_HDR_LEN + hdr[1] + RBF_PKG_FTR_LEN, 8);
	pr_info("[%u ms] q0 rx: read %#x write %#x, package header %#x length %u\n",
		elapsed, rd, wr, hdr[0], hdr[1]);

	if (hdr[0] != RBF_PKG_HEADER || pkg > avail || hdr[1] < CCCI_HEADER_LEN) {
		pr_err("q0 rx: not a complete CCIF package, skipping %u bytes\n", avail);
		WRITE_ONCE(*(u32 *)(rb + RBF_RX_READ), wr);
		return;
	}

	pos = (rd + RBF_PKG_HDR_LEN) % len;
	for (i = 0; i < 4; i++)
		hd[i] = READ_ONCE(*(u32 *)(buf + (pos + 4 * i) % len));
	pr_info("[%u ms] q0 rx: ccci_header data0 %#x data1 %#x channel %u seq %u reserved %#x\n",
		elapsed, hd[0], hd[1], hd[2] & 0xffff, (hd[2] >> 16) & 0x7fff, hd[3]);

	/* payload after the header: md_query_ap_feature for HS1 */
	words = min_t(u32, (hdr[1] - CCCI_HEADER_LEN) / 4, 8);
	for (i = 0; i < words; i++)
		pr_info("[%u ms] q0 rx: payload[%u] %#010x\n", elapsed, i,
			READ_ONCE(*(u32 *)(buf + (pos + CCCI_HEADER_LEN + 4 * i) % len)));

	if ((hd[2] & 0xffff) == CCCI_CONTROL_RX && hd[1] == MD_INIT_START_BOOT &&
	    hd[3] == MD_INIT_CHK_ID) {
		md->hs1 = true;
		pr_info("[%u ms] HS1 (MD_INIT_START_BOOT, %u bytes) received from the modem\n",
			elapsed, hdr[1]);
	}

	/* finish reading the package before handing the space back */
	mb();
	WRITE_ONCE(*(u32 *)(rb + RBF_RX_READ), (rd + pkg) % len);
}

static int r1md_wait_hs1(struct r1md *md)
{
	unsigned long start = jiffies;
	unsigned int timeout = min(hs1_timeout_ms, 60000U);
	unsigned int interval = clamp(poll_ms, 1U, 1000U);
	u32 sta0 = ~0, sta1 = ~0, trace[CCIF_BOOT_TRACE_LEN / 4];
	u32 last_trace[CCIF_BOOT_TRACE_LEN / 4] = { 0 };
	u32 seen = 0, rch, s0, s1;
	unsigned int elapsed, i, after_hs1 = 0;
	int ret = -ETIMEDOUT;

	pr_info("waiting up to %u ms for HS1, polling every %u ms\n", timeout,
		interval);

	for (;;) {
		elapsed = jiffies_to_msecs(jiffies - start);

		s0 = r1md_boot_sta(md, 0);
		s1 = r1md_boot_sta(md, 1);
		if (s0 != sta0 || s1 != sta1) {
			pr_info("[%u ms] MD boot status %#010x %#010x\n", elapsed, s0, s1);
			sta0 = s0;
			sta1 = s1;
		}

		for (i = 0; i < ARRAY_SIZE(trace); i++)
			trace[i] = readl(md->ap_ccif + APCCIF_CHDATA + 4 * i);
		if (memcmp(trace, last_trace, sizeof(trace))) {
			pr_info("[%u ms] CCIF boot trace %*ph\n", elapsed,
				(int)sizeof(trace), trace);
			memcpy(last_trace, trace, sizeof(trace));
		}

		rch = readl(md->ap_ccif + APCCIF_RCHNUM);
		if (rch) {
			if (rch & ~seen)
				pr_info("[%u ms] CCIF RCHNUM %#x (new %#x)\n", elapsed,
					rch, rch & ~seen);
			seen |= rch;
			/* only the low 16 channels are acked by the AP */
			writel(rch & 0xffff, md->ap_ccif + APCCIF_ACK);
			if (rch & BIT(CCIF_RX_EXCEPTION_INIT)) {
				pr_err("[%u ms] modem raised D2H_EXCEPTION_INIT: it hit an exception\n",
				       elapsed);
				ret = -EIO;
				break;
			}
		}

		r1md_check_rx_q0(md, elapsed);

		/* keep watching briefly after HS1 for follow-up traffic */
		if (md->hs1 && ++after_hs1 * interval >= 500)
			break;
		if (elapsed >= timeout)
			break;
		msleep(interval);
	}

	elapsed = jiffies_to_msecs(jiffies - start);
	if (md->hs1) {
		pr_info("RESULT: PASS, HS1 received; boot status %#x %#x, CCIF channels seen %#x, %u ms\n",
			sta0, sta1, seen, elapsed);
		return 0;
	}
	pr_err("RESULT: FAIL, no HS1 after %u ms; boot status %#x %#x, CCIF channels seen %#x\n",
	       elapsed, sta0, sta1, seen);
	return ret;
}

/* ------------------------------------------------------------------------
 * Driver
 */

static void r1md_power_off(struct r1md *md)
{
	int ret;

	if (md->clks_on) {
		r1md_ccif_reset(md);
		clk_bulk_disable_unprepare(ARRAY_SIZE(md->clks), md->clks);
		md->clks_on = false;
	}

	if (md->pd_on) {
		/* md_cd_power_off(): MTCMOS first, then srcclkena, then clocks */
		ret = pm_runtime_put_sync(md->dev);
		md->pd_on = false;
		if (ret < 0)
			pr_err("MD1 power domain off: %d\n", ret);
		if (r1md_md1_is_on(md->spm))
			pr_err("MD1 still powered after power off\n");
	}

	if (md->srcclkena_set) {
		regmap_update_bits(md->infracfg, INFRA_MD_SRCCLKENA, 0xff, 0);
		md->srcclkena_set = false;
	}

	regmap_set_bits(md->topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATE);

	if (md->vmodem_on) {
		regulator_disable(md->vmodem);
		md->vmodem_on = false;
	}

	r1md_unmap_mem(&md->nc_smem);
	r1md_dump_status("powered off", md->spm, md->infracfg, md->topckgen);
}

static int r1md_do_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct r1md *md;
	unsigned int i;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = dev;
	platform_set_drvdata(pdev, md);

	md->spm = syscon_regmap_lookup_by_compatible("mediatek,mt6765-scpsys");
	md->infracfg = syscon_regmap_lookup_by_phandle(np, "mediatek,infracfg");
	md->topckgen = syscon_regmap_lookup_by_phandle(np, "mediatek,topckgen");
	md->apmixed = syscon_regmap_lookup_by_phandle(np, "mediatek,apmixedsys");
	if (IS_ERR(md->spm) || IS_ERR(md->infracfg) || IS_ERR(md->topckgen) ||
	    IS_ERR(md->apmixed))
		return dev_err_probe(dev, -ENODEV, "missing syscon\n");

	md->ap_ccif = devm_platform_ioremap_resource_byname(pdev, "ap-ccif");
	if (IS_ERR(md->ap_ccif))
		return PTR_ERR(md->ap_ccif);
	md->md_ccif = devm_platform_ioremap_resource_byname(pdev, "md-ccif");
	if (IS_ERR(md->md_ccif))
		return PTR_ERR(md->md_ccif);
	md->boot_sta = devm_platform_ioremap_resource_byname(pdev, "md-boot-sta");
	if (IS_ERR(md->boot_sta))
		return PTR_ERR(md->boot_sta);

	for (i = 0; i < ARRAY_SIZE(md->clks); i++)
		md->clks[i].id = r1md_clk_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(md->clks), md->clks);
	if (ret)
		return dev_err_probe(dev, ret, "clocks\n");

	md->vmodem = devm_regulator_get(dev, "vmodem");
	if (IS_ERR(md->vmodem))
		return dev_err_probe(dev, PTR_ERR(md->vmodem), "vmodem\n");
	md->vcore = devm_regulator_get_optional(dev, "vcore");
	if (IS_ERR(md->vcore))
		md->vcore = NULL;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	r1md_dump_status("probe", md->spm, md->infracfg, md->topckgen);

	ret = r1md_power_on(md);
	if (ret) {
		pr_err("stage 1 failed (%d), powering MD1 down\n", ret);
		goto fail;
	}
	pr_info("stage 1 done: MD1 powered, PLLs up, MD WDT off, modem parked\n");
	if (stage < 2)
		return 0;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(md->clks), md->clks);
	if (ret) {
		pr_err("CCIF/CLDMA clocks: %d\n", ret);
		goto fail;
	}
	md->clks_on = true;

	ret = r1md_smem_prepare(md);
	if (ret)
		goto fail;
	r1md_ccif_reset(md);
	pr_info("CCIF AP RCHNUM %#x BUSY %#x, MD RCHNUM %#x\n",
		readl(md->ap_ccif + APCCIF_RCHNUM),
		readl(md->ap_ccif + APCCIF_BUSY),
		readl(md->md_ccif + APCCIF_RCHNUM));

	ret = r1md_let_md_go(md);
	if (ret) {
		pr_err("release failed (%d)\n", ret);
		goto fail;
	}

	ret = r1md_wait_hs1(md);
	r1md_dump_status("after wait", md->spm, md->infracfg, md->topckgen);
	if (!ret || keep_on_fail) {
		pr_info("modem left running; rmmod r1-md-probe powers it down\n");
		return 0;
	}
	pr_err("powering MD1 down\n");

fail:
	r1md_power_off(md);
	return ret;
}

static int r1md_probe(struct platform_device *pdev)
{
	r1md_result = r1md_do_probe(pdev);
	return r1md_result;
}

static void r1md_remove(struct platform_device *pdev)
{
	struct r1md *md = platform_get_drvdata(pdev);

	pr_info("powering MD1 down\n");
	r1md_power_off(md);
}

static const struct of_device_id r1md_of_match[] = {
	{ .compatible = "mediatek,mt6765-ccci" },
	{ }
};

/* no MODULE_DEVICE_TABLE: this must only ever be loaded by hand */

static struct platform_driver r1md_driver = {
	.probe = r1md_probe,
	.remove = r1md_remove,
	.driver = {
		.name = "r1-md-probe",
		.of_match_table = r1md_of_match,
	},
};

static int __init r1md_init(void)
{
	struct regmap *spm, *infracfg, *topckgen;
	int ret;

	pr_info("stage %u\n", stage);
	ret = r1md_read_handoff();
	if (ret)
		pr_err("LK modem handoff unusable (%d)\n", ret);

	spm = syscon_regmap_lookup_by_compatible("mediatek,mt6765-scpsys");
	infracfg = syscon_regmap_lookup_by_compatible("mediatek,mt6765-infracfg");
	topckgen = syscon_regmap_lookup_by_compatible("mediatek,mt6765-topckgen");
	if (IS_ERR(spm) || IS_ERR(infracfg) || IS_ERR(topckgen)) {
		pr_err("SoC syscons not found\n");
		return -ENODEV;
	}
	r1md_dump_status("boot state", spm, infracfg, topckgen);

	if (stage == 0) {
		pr_info("stage 0 done (read only); rmmod and reload with stage=1 or stage=2\n");
		return 0;
	}
	if (ret) {
		pr_err("not powering the modem without a valid handoff\n");
		return ret;
	}
	if (r1md_md1_is_on(spm)) {
		pr_err("MD1 is already powered; something else owns the modem\n");
		return -EBUSY;
	}

	/*
	 * Step 1 of md_cd_power_on(): MD clock mode. Done before the driver
	 * binds, because binding attaches and powers the MD1 domain.
	 */
	regmap_clear_bits(topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATE);
	pr_info("topckgen CLK_MODE %#x\n", r1md_rd(topckgen, TOPCKGEN_CLK_MODE));

	ret = platform_driver_register(&r1md_driver);
	if (ret)
		return ret;
	if (r1md_result) {
		if (r1md_result > 0) {
			pr_err("no mediatek,mt6765-ccci device probed; check the DT node and the MD1 power domain\n");
			r1md_result = -ENODEV;
		} else {
			pr_err("probe failed: %d\n", r1md_result);
		}
		platform_driver_unregister(&r1md_driver);
		regmap_set_bits(topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATE);
		r1md_dump_status("unloading", spm, infracfg, topckgen);
		return r1md_result;
	}
	return 0;
}
module_init(r1md_init);

static void __exit r1md_exit(void)
{
	if (stage > 0)
		platform_driver_unregister(&r1md_driver);
}
module_exit(r1md_exit);

MODULE_DESCRIPTION("Rabbit r1 MT6765 modem bring-up probe");
MODULE_LICENSE("GPL");
