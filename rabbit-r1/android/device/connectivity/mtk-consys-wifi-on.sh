#!/vendor/bin/sh
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Turn the CONSYS Wi-Fi function on. wmt_chrdev_wifi asks WMT to power it,
# WMT downloads the Wi-Fi ROM patch and calls the gen4m probe, and wlan0
# appears before the write returns. Errors are in the kernel log
# ("WMT turn on WIFI fail!").

echo 1 > /dev/wmtWifi
