#!/vendor/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Gate the modem userspace on the NV filesystems: start it only when nvdata,
# nvcfg, protect_f and protect_s are all mounted read-write as ext4 from
# their partitions. Otherwise leave the modem off and say why.

for mp in /mnt/vendor/nvdata /mnt/vendor/nvcfg /mnt/vendor/protect_f /mnt/vendor/protect_s; do
    line=$(grep " $mp ext4 " /proc/mounts)
    case "$line" in
    "/dev/block/"*" $mp ext4 rw,"*)
        ;;
    *)
        log -p e -t r1-modem "$mp is not mounted read-write; not starting the modem"
        exit 1
        ;;
    esac
done

setprop vendor.r1.modem.nv_ready 1
