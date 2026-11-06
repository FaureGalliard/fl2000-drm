#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# An initramfs to run the tests on the kernel the machine runs, whose 9p is a module:
#
#   mk-distro-initrd.sh /tmp/initrd.gz
#   qemu ... -kernel /boot/vmlinuz-linux -initrd /tmp/initrd.gz \
#       -append "console=ttyS0 panic=-1 fbcon=map:1 fl_arch=1 fl_ko=$PWD/fl2000.ko fl_suite=archall"
#
# with the -fsdev and -device options of run.sh. It needs dummy_hcd and raw_gadget among
# the modules of that kernel.

set -eu
OUT=$1
DIR=$(dirname "$(readlink -f "$0")")
KV=$(uname -r)
RD=$(mktemp -d)
trap 'rm -rf "$RD"' EXIT

mkdir -p "$RD"/usr/bin "$RD"/usr/lib "$RD"/proc "$RD"/sys "$RD"/dev "$RD"/newroot "$RD"/run
ln -s usr/bin "$RD/bin"
ln -s usr/lib "$RD/lib"
ln -s usr/lib "$RD/lib64"
for b in bash kmod mount switch_root sleep; do
	cp -L "$(command -v $b)" "$RD/usr/bin/"
	for l in $(ldd "$(command -v $b)" | grep -o '/[^ ]*'); do
		cp -Ln "$l" "$RD/usr/lib/" 2>/dev/null || true
	done
done
ln -s kmod "$RD/usr/bin/modprobe"
mkdir -p "$RD/lib/modules/$KV"
cp /lib/modules/"$KV"/modules.* "$RD/lib/modules/$KV/"
for m in $(modprobe --show-depends 9pnet_virtio | awk '/insmod/{print $2}') \
	 $(modprobe --show-depends 9p | awk '/insmod/{print $2}'); do
	d=$RD/lib/modules/$KV/${m#/lib/modules/"$KV"/}
	mkdir -p "$(dirname "$d")"
	cp -n "$m" "$d"
done
{
	echo '#!/usr/bin/bash'
	echo 'export PATH=/usr/bin'
	echo 'mount -t proc proc /proc'
	echo 'mount -t sysfs sys /sys'
	echo 'mount -t devtmpfs dev /dev'
	echo 'modprobe 9pnet_virtio'
	echo 'modprobe 9p'
	echo 'mount -t 9p -o ro,trans=virtio,version=9p2000.L,msize=1048576,cache=loose hostroot /newroot'
	echo "exec switch_root /newroot $DIR/init.sh"
} > "$RD/init"
chmod +x "$RD/init"
(cd "$RD" && find . | cpio -o -H newc --quiet | gzip -1) > "$OUT"
