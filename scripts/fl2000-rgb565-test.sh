#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Does the chip really take RGB565? Shows colour bars in XRGB8888 and RGB565 with the fl2000.ko
# of this tree and records, for each, the pixel format bit of the chip and the USB data rate.
# What is on the monitor can only be judged by looking at it. AR30 must be refused.
#
# From a TTY (Ctrl+Alt+F3), with the session saved, as it stops the display manager:
#
#   sudo bash scripts/fl2000-rgb565-test.sh
#
# The module is loaded from this tree and nothing is installed: at the end the installed module
# is loaded again and the display manager started.

set -u
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }

REPO=$(dirname "$(dirname "$(readlink -f "$0")")")
LOG=$REPO/docs/field-test-rgb565-$(date +%H_%M).log
HOLD=14

[ -f "$REPO/fl2000.ko" ] || { echo "no fl2000.ko: run make first"; exit 1; }

restore() {
	rmmod fl2000 2>/dev/null
	modprobe fl2000
	systemctl start gdm
	echo "installed module and gdm are back; log in $LOG"
}
trap restore EXIT

exec > >(tee "$LOG") 2>&1

echo "=== $(date), kernel $(uname -r), branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null)"
systemctl stop gdm
sleep 3

if ! rmmod fl2000; then
	echo "cannot unload the module: something still uses it"
	exit 1
fi
modprobe -a cec drm_display_helper i2c-dev
if ! insmod "$REPO/fl2000.ko" dyndbg=+p; then
	echo "cannot load $REPO/fl2000.ko"
	exit 1
fi
echo "srcversion $(cat /sys/module/fl2000/srcversion)"
sleep 8

modetest -M fl2000 -p | grep -m1 'formats:'
CONN=$(modetest -M fl2000 -c | awk '/\<connected\>/{print $1; exit}')
if [ -z "$CONN" ]; then
	echo "no monitor connected"
	exit 1
fi
modetest -M fl2000 -c | awk '/\<connected\>/{print; f=1} f && /#0 /{print; exit}'

show() {
	echo
	echo "=== $1: look at the monitor for $HOLD s"
	(sleep $HOLD | modetest -M fl2000 -s "$CONN:#0@$1" 2>&1 | grep -vE '^$') &
	sleep 7
	local pxclk
	pxclk=$(python3 "$REPO/scripts/fl2000-reg.py" read 0x8004 | grep -oE '0x[0-9a-fA-F]+' | tail -1)
	echo "PXCLK $pxclk, VGA565_MODE bit: $(( (pxclk >> 6) & 1 ))"
	wait
	dmesg | grep 'Stream:' | tail -1 | sed 's/^.*Stream:/Stream:/'
}

show XR24
show RG16
show XR24

echo
echo "=== AR30 must be refused"
sleep 3 | modetest -M fl2000 -s "$CONN:#0@AR30" 2>&1 | grep -vE '^$' | head -5

echo
echo "=== kernel complaints"
dmesg | grep -E 'fl2000|it66121|WARNING|BUG:' | grep -vE 'Stream:|Created FL2000|Detecting|found, rev|Creating the|Initialized|frame buffer|out-of-tree|verification|registered new|deregistering' | tail -15
