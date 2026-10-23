#!/bin/sh
# Run in a disposable Linux build container with matching headers and vmlinuz.
# Produces a RAM-only guest: no disk, host filesystem sharing, or networking.
set -eu
KERNEL=${1:?usage: build-initramfs.sh KERNEL_RELEASE}
cd "$(dirname "$0")/.."
make KDIR="/usr/src/linux-headers-$KERNEL"
mkdir -p vm/root/bin vm/root/sbin vm/root/proc vm/root/sys vm/root/dev
gcc -static -O2 -Wall -Wextra -Wno-unused-parameter -pthread tests/stress.c -o vm/root/stress
cp /bin/busybox vm/root/bin/busybox
for app in sh mount cat grep head seq sleep insmod rmmod uname poweroff true; do
    ln -sf /bin/busybox "vm/root/bin/$app"
done
cp proc_lens.ko vm/root/
cp tests/vm-init.sh vm/root/init
chmod +x vm/root/init
cp "/boot/vmlinuz-$KERNEL" vm/vmlinuz
(cd vm/root && find . -print0 | cpio --null -o --format=newc | gzip -1) > vm/initramfs.cpio.gz
