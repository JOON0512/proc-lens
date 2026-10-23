#!/bin/sh
export PATH=/bin:/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/console 2>&1
set -eu
fail() { echo "PROC_LENS_TEST_FAILED"; cat /hold.log 2>/dev/null || true; /bin/busybox dmesg | /bin/busybox tail -30; poweroff -f; }
trap fail EXIT
uname -a
if insmod /proc_lens.ko interval_ms=0 2>/dev/null; then exit 1; fi
if insmod /proc_lens.ko max_tasks=0 2>/dev/null; then exit 1; fi
if insmod /proc_lens.ko max_tasks=65537 2>/dev/null; then exit 1; fi
insmod /proc_lens.ko interval_ms=10 max_tasks=256
/stress
/stress hold >/hold.log 2>&1 &
holder=$!
for i in $(seq 1 100); do
    grep -q HOLD_READY /hold.log && break
    sleep 0.01
done
grep -q HOLD_READY /hold.log
rmmod proc_lens
wait "$holder"
cat /hold.log
test ! -e /proc/proc_lens
insmod /proc_lens.ko interval_ms=10 max_tasks=1
cat /proc/proc_lens/snapshot | head -1 | grep 'count=1 truncated=1'
rmmod proc_lens
for i in $(seq 1 50); do
    insmod /proc_lens.ko interval_ms=10 max_tasks=256
    for j in $(seq 1 10); do cat /proc/proc_lens/snapshot >/dev/null; done
    rmmod proc_lens
    test ! -e /proc/proc_lens
done
if /bin/busybox dmesg | grep -Ei 'BUG:|Oops:|WARNING:|use-after-free|kernel panic|sleeping function called'; then exit 1; fi
echo 'PASS: invalid parameters, capacity bound, 50 load/read/unload cycles, clean kernel log'
echo PROC_LENS_TEST_SUCCESS
trap - EXIT
poweroff -f
