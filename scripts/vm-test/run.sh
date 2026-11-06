#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the tests of this directory in a VM. See README.
#
#   run.sh <kernel-build-dir> <modules-install-dir> <log> [suite] [kernel args...]
#
# QEMU=/path/to/qemu-system-x86_64, and QEMU_DATA for its firmware directory, to use
# a QEMU that is not in the PATH.

set -eu
KB=$1; MODS=$2; LOG=$3; SUITE=${4:-all}
shift $(($# < 4 ? $# : 4))
DIR=$(dirname "$(readlink -f "$0")")
: "${QEMU:=qemu-system-x86_64}"

make -s -C "$DIR"

exec timeout "${VM_TIMEOUT:-1500}" "$QEMU" ${QEMU_DATA:+-L "$QEMU_DATA"} \
	-enable-kvm -cpu host -smp 4 -m 3G -nographic -no-reboot -nic none -vga none \
	-kernel "$KB/arch/x86/boot/bzImage" \
	-fsdev local,id=r,path=/,security_model=none,readonly=on,multidevs=remap \
	-device virtio-9p-pci,fsdev=r,mount_tag=hostroot \
	-append "console=ttyS0 root=hostroot rootfstype=9p \
rootflags=trans=virtio,version=9p2000.L,msize=1048576,cache=loose ro \
init=$DIR/init.sh panic=-1 dummy_hcd.is_super_speed=1 dummy_hcd.is_high_speed=1 \
fbcon=map:1 kmemleak=on fl_suite=$SUITE fl_mods=$MODS $*" > "$LOG" 2>&1
