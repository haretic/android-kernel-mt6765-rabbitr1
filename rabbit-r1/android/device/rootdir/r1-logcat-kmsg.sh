#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Debuggable builds only: mirror logcat into the kernel log, the one log a
# bring-up boot that resets can still be read back from.
[ "$(getprop ro.debuggable)" = 1 ] || exit 0
logcat -b main,system,crash -v brief '*:I' | while IFS= read -r line; do
    echo "logcat: $line" > /dev/kmsg
done
