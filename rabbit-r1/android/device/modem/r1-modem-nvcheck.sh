#!/vendor/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Gate the modem userspace on the NV filesystems: start it only when nvdata,
# nvcfg, protect_f and protect_s are all mounted read-write as ext4 from
# their partitions. Otherwise leave the modem off and say why.
#
# ccci_mdinit also needs the CCCI ports it opens at startup. ueventd creates
# them seconds after mtk-ccci registers them; an mdinit that comes first boots
# the modem without its CCB configuration (no /dev/ccci_ccb_ctrl), and the
# modem asserts in ccci_shm_bm.c and never finishes its handshake.

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

ports="/dev/ccci_monitor /dev/ccci_fs /dev/ccci_ipc_2 /dev/ccci_ipc_5 /dev/ccci_ccb_ctrl"
waited=0
for port in $ports; do
    while [ ! -c "$port" ]; do
        if [ $waited -ge 300 ]; then
            log -p e -t r1-modem "$port did not appear; not starting the modem"
            exit 1
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
done

setprop vendor.r1.modem.nv_ready 1
