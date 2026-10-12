# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(src)/inc -I$(src)/plat_inc
ccflags-y += -DBTIF_DBG_SUPPORT=0
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations -Wno-enum-conversion

obj-$(CONFIG_MTK_CONN_BTIF) += mtk_btif_drv.o
mtk_btif_drv-y := mtk_btif.o mtk_btif_exp.o btif_dma_plat.o btif_plat.o
