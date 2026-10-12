// SPDX-License-Identifier: GPL-2.0
/*
 * In-kernel stand-in for MediaTek's Android wmt_loader + wmt_launcher.
 *
 * The stock driver only creates /dev/wmtdetect at load time and leaves
 * the rest of bring-up to two vendor daemons:
 *
 *   wmt_loader:   COMBO_IOCTL_CONNSYS_SOC_HW_INIT, GET_SOC_CHIP_ID,
 *                 SET_CHIP_ID, DO_MODULE_INIT (creates /dev/stpwmt)
 *   wmt_launcher: WMT_IOCTL_SET_STP_MODE (BTIF full mode, FM over the
 *                 common interface), SET_LAUNCHER_KILL(0), and then
 *                 answers the core's uplink commands while CONSYS powers
 *                 on: "srh_rom_patch", "srh_patch", "update_patch_version".
 *
 * Doing the same here means the board needs no proprietary daemon: only
 * the firmware files under the firmware_class search path. The uplink
 * answers mirror the stock launcher: firmware header u2SwVer (offset 22)
 * must match the chip FW version, the patch address is at offset 24, its
 * low byte carries "total << 4 | sequence" and is cleared before use.
 *
 * Disable with wmt_drv.autolaunch=0 to use the stock daemons instead.
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include "osal_typedef.h"
#include "osal.h"
#include "wmt_core.h"
#include "wmt_lib.h"
#include "wmt_dev.h"
#include "wmt_plat.h"
#include "wmt_detect.h"
#include <mtk_wcn_cmb_stub.h>

static bool autolaunch = IS_ENABLED(CONFIG_MTK_CONN_WMT_AUTOLAUNCH);
module_param(autolaunch, bool, 0444);
MODULE_PARM_DESC(autolaunch,
		 "Bring up WMT at load and answer the launcher's uplink commands in-kernel (default from Kconfig)");

#define WMT_KLAUNCH_CHIPID	0x6765
/* STP over BTIF (full mode), FM over the common interface */
#define WMT_KLAUNCH_STP_MODE	((WMT_FM_COMM << 4) | STP_BTIF_FULL)
#define WMT_KLAUNCH_CFG		"WMT_SOC.cfg"

/* struct WMT_PATCH / struct wmt_rom_patch header layout */
#define WMT_PATCH_HDR_SWVER_OFF	22
#define WMT_PATCH_HDR_ADDR_OFF	24
#define WMT_PATCH_HDR_MIN_LEN	(WMT_PATCH_HDR_ADDR_OFF + 4)

/* MT6765 CONSYS (CONNAC1 "soc1_0"), names as shipped in /vendor/firmware */
static const char * const wmt_klaunch_patches[] = {
	"soc1_0_patch_mcu_1_1_hdr.bin",
};

static const struct {
	ENUM_WMTDRV_TYPE_T type;
	const char *name;
} wmt_klaunch_rom_patches[] = {
	{ WMTDRV_TYPE_BT, "soc1_0_ram_bt_1_1_hdr.bin" },
	{ WMTDRV_TYPE_WIFI, "soc1_0_ram_wifi_1_1_hdr.bin" },
	{ WMTDRV_TYPE_WMT, "soc1_0_ram_mcu_1_1_hdr.bin" },
};

/* handed to the core by pointer and never freed by it */
static WMT_PATCH_INFO wmt_klaunch_patch_info[MAX_PATCH_NUM];

MTK_WCN_BOOL wmt_klaunch_enabled(VOID)
{
	return autolaunch ? MTK_WCN_BOOL_TRUE : MTK_WCN_BOOL_FALSE;
}

static INT32 wmt_klaunch_read_hdr(const char *name, UINT8 *hdr)
{
	const struct firmware *fw = NULL;
	INT32 ret;

	ret = firmware_request_nowarn(&fw, name, NULL);
	if (ret) {
		WMT_ERR_FUNC("klaunch: cannot load %s (%d)\n", name, ret);
		return ret;
	}

	if (fw->size < WMT_PATCH_HDR_MIN_LEN) {
		WMT_ERR_FUNC("klaunch: %s is too short (%zu bytes)\n", name, fw->size);
		release_firmware(fw);
		return -EINVAL;
	}

	memcpy(hdr, fw->data, WMT_PATCH_HDR_MIN_LEN);
	release_firmware(fw);
	return 0;
}

static INT32 wmt_klaunch_srh_patch(VOID)
{
	UINT32 fw_ver = wmt_lib_get_icinfo(WMTCHIN_FWVER);
	UINT8 major = (fw_ver >> 8) & 0xff, minor = fw_ver & 0xff;
	UINT8 hdr[WMT_PATCH_HDR_MIN_LEN];
	UINT32 total = 0, found = 0, seq, i;

	WMT_INFO_FUNC("klaunch: srh_patch, chip FW version 0x%04x\n", fw_ver);
	memset(wmt_klaunch_patch_info, 0, sizeof(wmt_klaunch_patch_info));

	for (i = 0; i < ARRAY_SIZE(wmt_klaunch_patches); i++) {
		const char *name = wmt_klaunch_patches[i];
		WMT_PATCH_INFO *info;

		if (wmt_klaunch_read_hdr(name, hdr))
			continue;

		if (hdr[WMT_PATCH_HDR_SWVER_OFF] != major ||
		    hdr[WMT_PATCH_HDR_SWVER_OFF + 1] != minor) {
			WMT_ERR_FUNC("klaunch: %s is for FW 0x%02x%02x, chip has 0x%04x, skipped\n",
				     name, hdr[WMT_PATCH_HDR_SWVER_OFF],
				     hdr[WMT_PATCH_HDR_SWVER_OFF + 1], fw_ver);
			continue;
		}

		if (!total) {
			total = (hdr[WMT_PATCH_HDR_ADDR_OFF] & 0xf0) >> 4;
			if (!total || total > MAX_PATCH_NUM) {
				WMT_ERR_FUNC("klaunch: %s claims %u patches\n", name, total);
				return -EINVAL;
			}
		}

		seq = hdr[WMT_PATCH_HDR_ADDR_OFF] & 0x0f;
		if (!seq || seq > total || wmt_klaunch_patch_info[seq - 1].dowloadSeq) {
			WMT_ERR_FUNC("klaunch: %s has bad sequence %u/%u\n", name, seq, total);
			return -EINVAL;
		}

		info = &wmt_klaunch_patch_info[seq - 1];
		info->dowloadSeq = seq;
		memcpy(info->addRess, &hdr[WMT_PATCH_HDR_ADDR_OFF], sizeof(info->addRess));
		info->addRess[0] = 0;	/* sequence data, not address bits */
		strscpy(info->patchName, name, sizeof(info->patchName));
		found++;

		WMT_INFO_FUNC("klaunch: patch %u/%u %s addr 0x%02x%02x%02x%02x\n",
			      seq, total, name, info->addRess[3], info->addRess[2],
			      info->addRess[1], info->addRess[0]);
	}

	if (!total || found != total) {
		WMT_ERR_FUNC("klaunch: found %u of %u MCU patches, CONSYS will not power on\n",
			     found, total);
		return -ENOENT;
	}

	wmt_lib_set_patch_num(total);
	wmt_lib_set_patch_info(wmt_klaunch_patch_info);
	return 0;
}

static INT32 wmt_klaunch_srh_rom_patch(VOID)
{
	struct wmt_rom_patch_info info;
	UINT8 hdr[WMT_PATCH_HDR_MIN_LEN];
	INT32 ret = 0;
	UINT32 i;

	WMT_INFO_FUNC("klaunch: srh_rom_patch\n");

	for (i = 0; i < ARRAY_SIZE(wmt_klaunch_rom_patches); i++) {
		const char *name = wmt_klaunch_rom_patches[i].name;

		if (wmt_klaunch_read_hdr(name, hdr)) {
			/* the core cannot boot without the MCU RAM code */
			if (wmt_klaunch_rom_patches[i].type == WMTDRV_TYPE_WMT)
				ret = -ENOENT;
			continue;
		}

		memset(&info, 0, sizeof(info));
		info.type = wmt_klaunch_rom_patches[i].type;
		memcpy(info.addRess, &hdr[WMT_PATCH_HDR_ADDR_OFF], sizeof(info.addRess));
		/*
		 * The low byte is a sequence flag; the core adds the address
		 * to the EMI base as-is, so a stale 0x11 there puts the RAM
		 * code at the wrong offset and the MCU never boots.
		 */
		info.addRess[0] = 0;
		strscpy(info.patchName, name, sizeof(info.patchName));

		WMT_INFO_FUNC("klaunch: rom patch type %u %s addr 0x%02x%02x%02x%02x\n",
			      info.type, name, info.addRess[3], info.addRess[2],
			      info.addRess[1], info.addRess[0]);
		wmt_lib_set_rom_patch_info(&info, info.type);
	}

	return ret;
}

/*
 * Called from wmt_ctrl_ul_cmd() on the WMT thread in place of signalling
 * the userspace launcher. Returns the value the launcher would have
 * written back as the command result.
 */
INT32 wmt_klaunch_ul_cmd(const PUINT8 pCmdStr)
{
	const char *cmd = (const char *)pCmdStr;

	if (!strcmp(cmd, "srh_patch"))
		return wmt_klaunch_srh_patch();
	if (!strcmp(cmd, "srh_rom_patch"))
		return wmt_klaunch_srh_rom_patch();
	if (!strcmp(cmd, "update_patch_version")) {
		/* stock launcher only mirrors the version into an Android property */
		return 0;
	}

	/* open_stp/close_stp/baud_* only exist for UART-attached combo chips */
	WMT_ERR_FUNC("klaunch: unsupported uplink command \"%s\"\n", cmd);
	return -EOPNOTSUPP;
}

/*
 * Run once from module init. Failures are logged and the module stays
 * loaded (with /dev/wmtdetect) so the stock daemons or a retry after
 * rmmod can still be used.
 */
INT32 wmt_klaunch_start(VOID)
{
	const struct firmware *cfg = NULL;
	UINT32 chipid;
	INT32 ret;

	if (!autolaunch) {
		WMT_INFO_FUNC("klaunch: disabled, waiting for the userspace launcher\n");
		return 0;
	}

	ret = wmt_plat_consys_hw_init();
	if (ret) {
		WMT_ERR_FUNC("klaunch: CONSYS platform init failed (%d), WMT not started\n", ret);
		return ret;
	}

	chipid = wmt_plat_get_soc_chipid();
	if (chipid != WMT_KLAUNCH_CHIPID) {
		WMT_ERR_FUNC("klaunch: unexpected SoC chip id 0x%x (want 0x%x), WMT not started\n",
			     chipid, WMT_KLAUNCH_CHIPID);
		return -ENODEV;
	}
	mtk_wcn_wmt_set_chipid(chipid);
	wmt_detect_set_chip_type(chipid);

	/*
	 * wmt_lib_init() fails hard without the config, and its error path
	 * tears down the platform driver. Check first so the message says
	 * what is actually wrong.
	 */
	ret = firmware_request_nowarn(&cfg, WMT_KLAUNCH_CFG, NULL);
	if (ret) {
		WMT_ERR_FUNC("klaunch: %s not found (%d); load wmt_drv after the firmware partition is mounted\n",
			     WMT_KLAUNCH_CFG, ret);
		return ret;
	}
	release_firmware(cfg);

	ret = mtk_wcn_common_drv_init();
	if (ret) {
		WMT_ERR_FUNC("klaunch: WMT core init failed (%d)\n", ret);
		return ret;
	}

	ret = wmt_dev_set_stp_mode(WMT_KLAUNCH_STP_MODE);
	if (ret) {
		WMT_ERR_FUNC("klaunch: setting STP mode 0x%x failed (%d)\n",
			     WMT_KLAUNCH_STP_MODE, ret);
		return ret;
	}
	wmt_lib_set_stp_wmt_last_close(0);

	WMT_INFO_FUNC("klaunch: WMT ready for chip 0x%x (CONSYS powers on with the first function)\n",
		      chipid);
	return 0;
}
