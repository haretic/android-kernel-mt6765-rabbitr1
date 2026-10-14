# SPDX-License-Identifier: Apache-2.0
# Explicit experimental product; the installer verifies its kernel SOC config.
$(call inherit-product, device/rabbit/r1/lineage_r1.mk)
PRODUCT_NAME := lineage_r1_soc
# Apps show "<manufacturer> <model>": haretic rabbit r1.
PRODUCT_MANUFACTURER := haretic
PRODUCT_MODEL := rabbit r1
