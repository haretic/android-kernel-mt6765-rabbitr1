#!/vendor/bin/sh
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Turn the CONSYS Wi-Fi function on. wmt_chrdev_wifi asks WMT to power it,
# WMT downloads the Wi-Fi ROM patch and calls the gen4m probe, and wlan0
# appears before the write returns. Errors are in the kernel log
# ("WMT turn on WIFI fail!").
#
# ueventd creates /dev/wmtWifi asynchronously, and while it relabels sysfs
# during boot the node can appear several seconds after wmt_chrdev_wifi loads.

i=0
while [ ! -c /dev/wmtWifi ] && [ $i -lt 300 ]; do
    sleep 0.1
    i=$((i + 1))
done
echo 1 > /dev/wmtWifi
