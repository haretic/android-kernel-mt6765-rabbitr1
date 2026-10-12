# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_MTK_CCCI_R1_MD_PROBE) += r1-md-probe.o
r1-md-probe-y := r1_md_probe.o

obj-$(CONFIG_MTK_CCCI) += mtk-ccci.o
mtk-ccci-y := \
	mtk_ccci_main.o \
	ccci_util/ccci_util_lib_fo.o \
	ccci_util/ccci_util_lib_load_img.o \
	ccci_util/ccci_util_lib_sys.o \
	ccci_util/ccci_private_log.o \
	ccci_util/ccci_util_lib_time.o \
	ccci_util/ccci_util_lib_main.o \
	ccci_util/ccci_util_ld_md_errno.o \
	ccci_util/ccci_util_broadcast.o \
	ccci_util/ccci_util_pin_broadcast.o \
	ccci_util/ccci_util_sib.o \
	ccci_util/ccci_util_md_rat.o \
	ccci_util/ccci_util_dummy.o \
	ccmni/ccmni.o \
	eccci/ccci_core.o \
	eccci/ccci_bm.o \
	eccci/ccci_modem.o \
	eccci/modem_sys1.o \
	eccci/ccci_auxadc.o \
	eccci/fsm/ccci_fsm.o \
	eccci/fsm/ccci_fsm_ioctl.o \
	eccci/fsm/ccci_fsm_monitor.o \
	eccci/fsm/ccci_fsm_poller.o \
	eccci/fsm/ccci_fsm_scp.o \
	eccci/fsm/ccci_fsm_ee.o \
	eccci/fsm/ccci_fsm_sys.o \
	eccci/fsm/mdee_dumper_v3.o \
	eccci/port/port_cfg.o \
	eccci/port/port_char.o \
	eccci/port/port_ctlmsg.o \
	eccci/port/port_udc.o \
	eccci/port/port_misc.o \
	eccci/port/port_ipc.o \
	eccci/port/port_net.o \
	eccci/port/port_poller.o \
	eccci/port/port_proxy.o \
	eccci/port/port_rpc.o \
	eccci/port/port_smem.o \
	eccci/port/port_sysmsg.o \
	eccci/port/ccci_swtp.o \
	eccci/udc/udc.o \
	eccci/hif/ccci_hif.o \
	eccci/hif/ccci_ringbuf.o \
	eccci/hif/ccci_debug_info.o \
	eccci/hif/ccci_hif_ccif.o \
	eccci/hif/ccci_cldma_plat.o \
	eccci/hif/ccci_hif_cldma.o \
	eccci/mt6765/ccci_platform.o \
	eccci/mt6765/md_sys1_platform.o

# The vendor sources expect the include layout of the MediaTek tree and a
# handful of vendor kernel interfaces, provided by mtk_ccci_compat.h.
mtk-ccci-flags := \
	-include $(src)/mtk_ccci_compat.h \
	-I$(src)/include \
	-I$(src)/ccci_util \
	-I$(src)/ccmni \
	-I$(src)/eccci \
	-I$(src)/eccci/inc \
	-I$(src)/eccci/fsm \
	-I$(src)/eccci/port \
	-I$(src)/eccci/hif \
	-I$(src)/eccci/mt6765 \
	-I$(src)/eccci/udc \
	-I$(src)/eccci/udc/udc_lib_inc \
	-Wno-missing-prototypes \
	-Wno-missing-declarations

$(foreach o,$(mtk-ccci-y),$(eval CFLAGS_$(o) += $(mtk-ccci-flags)))
