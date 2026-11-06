#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PID 1 of the test VM, see README. The root file system is the host's, read only,
# and everything written goes to a tmpfs. Prints PASS or FAIL for each check.
export PATH=/usr/bin
T=$(dirname "$(readlink -f "$0")")
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
mount -t debugfs dbg /sys/kernel/debug
mount -t tmpfs run /run
mkdir -p /dev/pts /dev/shm
cd /run
for a in $(cat /proc/cmdline); do case $a in fl_suite=*) SUITE=${a#fl_suite=};; fl_mods=*) MODS=${a#fl_mods=};; fl_arch=*) ARCHK=1;; fl_ko=*) FLKO=${a#fl_ko=};; esac; done
SUITE=${SUITE:-all}
exec 2>&1
fails=0
say() { echo "### $*"; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails+1)); }
scan() {
	# Kernel complaints since the last call
	dmesg | grep -E 'BUG:|WARNING:|Oops|KASAN|UBSAN|possible .*deadlock|circular locking|inconsistent lock|blocked for more|kernel BUG|corruption|ODEBUG|Unable to handle|general protection|BOGUS' > /run/bad.txt
	if [ -s /run/bad.txt ]; then fail "kernel complained in: $1"; head -12 /run/bad.txt; else pass "no kernel complaints: $1"; fi
	dmesg -C
}
card() { local c; for c in /sys/class/drm/card[0-9]; do [ "$(basename $(readlink -f $c/device/driver 2>/dev/null) 2>/dev/null)" = fl2000 ] && { echo /dev/dri/$(basename $c); return 0; }; done; return 1; }
cardname() { local c=$(card) && basename $c; }
wait_card() { for i in $(seq 1 ${1:-300}); do card >/dev/null && return 0; sleep 0.1; done; return 1; }
wait_nocard() { for i in $(seq 1 ${1:-200}); do card >/dev/null || return 0; sleep 0.1; done; return 1; }
emu_start() { rm -f /run/ctl /run/emu.log; mkfifo /run/ctl; (exec 3<>/run/ctl; sleep 100000) & CTLH=$!; $T/fl2000-emu --ctl /run/ctl --log /run/emu.log "$@" & EMU=$!; }
emu_stop() { kill -9 $EMU 2>/dev/null; wait $EMU 2>/dev/null; kill $CTLH 2>/dev/null; wait $CTLH 2>/dev/null; EMU=; }
ctl() { echo "$*" > /run/ctl; }
stats() { ctl stats; sleep 0.3; grep frames= /run/emu.log | tail -1; }
frames() { ctl stats; sleep 0.3; grep -o 'frames=[0-9]*' /run/emu.log | tail -1 | cut -d= -f2; }
conn() { echo /sys/class/drm/$(cardname)-$1; }
detect() { echo detect > $(conn $1)/status; cat $(conn $1)/status; }
show_verify() { # w h fbw fbh sx sy fmt bytes_pix
	$T/fl2000-t show $(card) $1 $2 $3 $4 $5 $6 ${HOLD:-4} $7 & S=$!
	sleep 3
	local f=$(frames)
	ctl dump /run/frame.bin; sleep 0.5
	if [ "${f:-0}" -gt 0 ]; then pass "frames arrive ($f) for $1x$2"; else fail "no frames for $1x$2"; fi
	python3 -I $T/verify_frame.py /run/frame.bin $1 $2 $5 $6 $8 && pass "frame content $1x$2 fb $3x$4 at $5,$6 $7" || fail "frame content $1x$2 fb $3x$4 at $5,$6 $7"
	wait $S
}
load() {
	if [ -n "$ARCHK" ]; then
		modprobe -a cec drm_display_helper i2c-dev
		insmod $FLKO dyndbg=+p || fail "insmod"
	elif [ -n "$FLKO" ]; then
		modprobe -d $MODS fl2000 2>/dev/null; rmmod fl2000 2>/dev/null
		insmod $FLKO dyndbg=+p || fail "insmod"
	else
		modprobe -d $MODS fl2000 dyndbg=+p || fail "modprobe"
	fi
}
unload() { rmmod fl2000 && pass "rmmod" || fail "rmmod"; }

[ -n "$ARCHK" ] && { modprobe dummy_hcd is_super_speed=1 is_high_speed=1; modprobe raw_gadget; }
dmesg -C
echo 8 > /proc/sys/kernel/printk
say "kernel $(uname -r), module ${FLKO:-installed}, suite $SUITE"

basic() {
	say "T01 plug: probe through one interface, device, connector, EDID"
	load
	emu_start
	wait_card && pass "DRM device appears ($(card))" || { fail "no DRM device"; dmesg | tail -40; return; }
	sleep 1	# the device is up from the probe of interface 0, the others register right after
	ls /sys/bus/usb/drivers/fl2000/ | grep -c ':1\.' | grep -q 3 && pass "three interfaces bound" || fail "interfaces: $(ls /sys/bus/usb/drivers/fl2000/)"
	[ "$(cat $(conn HDMI-A-1)/status)" = connected ] && pass "HDMI connected" || fail "HDMI status $(cat $(conn HDMI-A-1)/status)"
	[ "$(wc -c < $(conn HDMI-A-1)/edid)" = 256 ] && pass "EDID 256 bytes" || fail "EDID size $(wc -c < $(conn HDMI-A-1)/edid)"
	dmesg | grep -E 'IT66121 found' || fail "chip not detected on the bus"
	scan T01

	say "T02 modes"
	modetest -M fl2000 -c 2>&1 | grep -E 'HDMI|VGA|1920x1080' | head -6
	scan T02

	say "T03 frames reach the device, correct and word-swapped"
	show_verify 1920 1080 1920 1080 0 0 xr24 3
	show_verify 1366 768 1366 768 0 0 ar30 3
	scan T03

	say "T04 framebuffer larger than the mode, with a source offset"
	show_verify 800 600 1000 700 37 21 xr24 3
	show_verify 1024 768 1200 800 3 5 ar30 3
	scan T04

	say "T05 cursor plane"
	$T/fl2000-t cursor $(card) 3 && pass "cursor commits" || fail "cursor commits"
	scan T05

	say "T06 a huge mode with the primary plane off screen"
	$T/fl2000-t hugemode $(card) && pass "huge mode rejected" || fail "huge mode"
	scan T06

	say "T08 page flips with vsync, the way a compositor does it"
	modetest -M fl2000 -c 2>&1 | sed -n 1,12p | cut -c1-70
	C=$(modetest -M fl2000 -c 2>/dev/null | awk '$3 == "connected" {print $1; exit}')
	echo "connector $C"
	timeout 6 modetest -M fl2000 -s $C:#0 -v 2>&1 | tail -3 | cut -c1-80
	scan T08

	say "T09 atomic flips with events, and damage clips on a framebuffer larger than the mode"
	$T/fl2000-t flips $(card) 4 && pass "atomic flips" || fail "atomic flips"
	for r in "100 80 300 200" "20 10 60 40" "830 600 1000 700" "37 21 41 22" "0 0 1000 700"; do
		set -- $r
		$T/fl2000-t damage $(card) 800 600 1000 700 37 21 $1 $2 $3 $4 4 & S=$!
		sleep 3
		ctl dump /run/frame.bin; sleep 0.5
		python3 -I $T/verify_frame.py /run/frame.bin 800 600 37 21 3 $1 $2 $3 $4 && pass "damage clip $r" || fail "damage clip $r"
		wait $S
	done
	scan T09

	say "T07 I2C adapter validation through i2c-dev"
	I=$(grep -l 'FL2000 bridge' /sys/bus/i2c/devices/i2c-*/name | head -1 | xargs dirname | xargs basename)
	$T/fl2000-t i2c0 /dev/$I && pass "bad I2C messages rejected, good one reads the ID" || fail "i2c validation"
	scan T07
}

hotplug() {
	say "T10 monitor events"
	local c0
	ctl hdmi 0; sleep 1
	grep -c 'interrupt sent' /run/emu.log | grep -qv '^0$' && pass "interrupt reaches the endpoint" || echo "note: no interrupt sent yet"
	[ "$(detect HDMI-A-1)" = disconnected ] && pass "HDMI gone" || fail "HDMI still there"
	[ "$(detect VGA-1)" = disconnected ] && pass "no VGA either" || fail "VGA with nothing plugged in"
	stats
	ctl vga 1; sleep 1
	[ "$(detect VGA-1)" = connected ] && pass "VGA found" || fail "VGA not found"
	ctl vga 0; sleep 1
	[ "$(detect VGA-1)" = disconnected ] && pass "VGA gone" || fail "VGA still there"
	ctl hdmi 1; sleep 1
	[ "$(detect HDMI-A-1)" = connected ] && pass "HDMI back" || fail "HDMI not back"
	stats
	scan T10

	say "T11 HDMI unplugged while being driven: no phantom VGA monitor"
	HOLD=8 $T/fl2000-t show $(card) 1024 768 1024 768 0 0 8 xr24 & S=$!
	sleep 2
	ctl hdmi 0; sleep 1
	[ "$(detect HDMI-A-1)" = disconnected ] && pass "HDMI gone while driven" || fail "HDMI still there"
	[ "$(detect VGA-1)" = disconnected ] && pass "no phantom VGA" || fail "PHANTOM VGA while the sense is forced"
	wait $S; sleep 0.5
	ctl vga 1; sleep 0.5
	[ "$(detect VGA-1)" = connected ] && pass "real VGA seen once the CRTC is off" || fail "real VGA not seen"
	ctl vga 0; ctl hdmi 1; sleep 1
	detect HDMI-A-1 >/dev/null
	scan T11
}

lifetime() {
	say "T20 unplug while streaming, with the file still open"
	$T/fl2000-t show $(card) 1920 1080 1920 1080 0 0 12 xr24 & S=$!
	sleep 3
	emu_stop
	wait_nocard && pass "device gone after unplug" || fail "device still there, hub worker stuck?"
	sleep 1
	kill $S 2>/dev/null; wait $S 2>/dev/null
	sleep 1
	scan T20

	say "T21 replug"
	emu_start
	wait_card && pass "device back after replug ($(card))" || fail "no device after replug"
	show_verify 1024 768 1024 768 0 0 xr24 3
	scan T21

	say "T22 rmmod with the adapter plugged in, then unplug (crashed: devres on the usb_device)"
	unload
	sleep 0.5
	ls /sys/bus/usb/drivers/ | grep -q fl2000 && fail "driver still registered"
	grep -l 'FL2000 bridge' /sys/bus/i2c/devices/i2c-*/name 2>/dev/null && fail "I2C adapter left behind" || pass "no I2C adapter left behind"
	ls /sys/bus/i2c/devices | grep -c 004c | grep -q '^0$' && pass "no I2C client left behind" || fail "I2C client left behind"
	emu_stop
	sleep 1
	pass "unplug after rmmod survived"
	scan T22

	say "T23 module loaded with the adapter already there"
	emu_start; sleep 2
	load
	wait_card && pass "binds when loaded late" || fail "no device when loaded late"
	scan T23

	say "T24 sysfs unbind and bind of interface 0, twice"
	U=$(ls /sys/bus/usb/drivers/fl2000 | grep ':1\.0$')
	for n in 1 2; do
		echo $U > /sys/bus/usb/drivers/fl2000/unbind
		wait_nocard && pass "unbound" || fail "still bound"
		ls /sys/bus/usb/drivers/fl2000 | grep -c ':1\.' | grep -q '^0$' && pass "all interfaces let go" || fail "left: $(ls /sys/bus/usb/drivers/fl2000)"
		echo $U > /sys/bus/usb/drivers/fl2000/bind
		wait_card && pass "bound again" || fail "not bound again"
	done
	scan T24

	say "T25 unbinding a claimed interface takes everything down, cleanly"
	V=$(ls /sys/bus/usb/drivers/fl2000 | grep ':1\.2$')
	echo $V > /sys/bus/usb/drivers/fl2000/unbind
	wait_nocard && pass "gone with interface 2" || fail "still there"
	ls /sys/bus/usb/drivers/fl2000 | grep -c ':1\.' | grep -q '^0$' && pass "all interfaces let go" || fail "left: $(ls /sys/bus/usb/drivers/fl2000)"
	echo $U > /sys/bus/usb/drivers/fl2000/bind
	wait_card && pass "bound again" || fail "not bound again"
	scan T25

	say "T26 rebind with the old device held open (was -EBUSY from drm_bridge_attach)"
	OLD=$(card)
	$T/fl2000-t show $OLD 1024 768 1024 768 0 0 15 xr24 & S=$!
	sleep 3
	echo $U > /sys/bus/usb/drivers/fl2000/unbind
	wait_nocard && pass "unbound with the file open and streaming" || fail "still bound"
	echo $U > /sys/bus/usb/drivers/fl2000/bind
	wait_card && pass "new device with the old one still open ($(card))" || fail "no new device: bridge busy?"
	dmesg | grep -E 'Cannot attach|EBUSY' && fail "attach error"
	$T/fl2000-t hold $(card) 1
	kill $S 2>/dev/null; wait $S 2>/dev/null
	sleep 1
	scan T26

	say "T27 USB reset while streaming: same device, no unbind, frames go on"
	C=$(card)
	$T/fl2000-t show $C 1024 768 1024 768 0 0 20 xr24 & S=$!
	sleep 3
	f0=$(frames)
	B=$(cat /sys/bus/usb/devices/${U%:*}/busnum); D=$(cat /sys/bus/usb/devices/${U%:*}/devnum)
	$T/fl2000-t reset $B $D && pass "USBDEVFS_RESET" || fail "USBDEVFS_RESET"
	sleep 5
	[ "$(card)" = "$C" ] && pass "same DRM device after the reset" || fail "device changed: $C -> $(card)"
	f1=$(frames)
	[ "${f1:-0}" -gt "${f0:-0}" ] && pass "frames go on after the reset ($f0 -> $f1)" || fail "no frames after the reset ($f0 -> $f1)"
	stats
	ctl dump /run/frame.bin; sleep 0.5
	python3 -I $T/verify_frame.py /run/frame.bin 1024 768 0 0 3 && pass "frame content after the reset" || fail "frame content after the reset"
	kill $S 2>/dev/null; wait $S 2>/dev/null
	sleep 1
	scan T27
}

faults() {
	say "T30 allocation failures in the mode set (fail-nth)"
	echo Y > /sys/kernel/debug/failslab/task-filter 2>/dev/null
	echo N > /sys/kernel/debug/failslab/ignore-gfp-wait 2>/dev/null
	echo N > /sys/kernel/debug/fail_page_alloc/ignore-gfp-wait 2>/dev/null
	echo N > /sys/kernel/debug/fail_page_alloc/ignore-gfp-highmem 2>/dev/null
	echo 0 > /sys/kernel/debug/fail_page_alloc/min-order 2>/dev/null
	echo 0 > /sys/kernel/debug/failslab/verbose 2>/dev/null
	echo 0 > /sys/kernel/debug/fail_page_alloc/verbose 2>/dev/null
	dmesg -n 4
	$T/fl2000-t failnth $(card) 1 ${FAILN:-120} && pass "fail-nth loop, the start" || fail "fail-nth loop, the start"
	# 6 buffers of 1519 pages at 1080p: then the shadow, and the end of the vmalloc
	$T/fl2000-t failnth $(card) 9140 9200 && pass "fail-nth loop, the shadow" || fail "fail-nth loop, the shadow"
	$T/fl2000-t failnth $(card) 10640 10700 && pass "fail-nth loop, the end of it" || fail "fail-nth loop, the end of it"
	dmesg -n 8
	dmesg | grep -c 'Cannot allocate the stream'
	scan T30
	say "T31 still works after all that"
	HOLD=4 show_verify 1024 768 1024 768 0 0 xr24 3
	scan T31
}

variants() {
	say "T40 no IT66121 on the bus: a clean failed probe"
	emu_stop; sleep 0.5
	emu_start --no-it66121
	sleep 3
	card >/dev/null && fail "bound with no chip" || pass "does not bind without the chip"
	dmesg | grep -E 'No IT66121 found' && pass "says why" || fail "no message"
	grep -l 'FL2000 bridge' /sys/bus/i2c/devices/i2c-*/name 2>/dev/null && fail "I2C adapter left behind" || pass "no I2C adapter left behind"
	emu_stop; sleep 1
	scan T40

	say "T41 the chip does not answer at first"
	emu_start --i2c-fail 3
	wait_card && pass "binds after failed I2C reads" || fail "gave up too early"
	scan T41
	emu_stop; wait_nocard; sleep 0.5

	say "T42 high speed: RGB565 at 640x480"
	emu_start --high-speed
	wait_card && pass "binds on high speed" || fail "no device on high speed"
	show_verify 640 480 640 480 0 0 xr24 2
	$T/fl2000-t cursor $(card) 2 640 480 25000 && pass "cursor at RGB565" || fail "cursor at RGB565"
	scan T42
	emu_stop; wait_nocard; sleep 0.5
}

stress() {
	say "T60 unplug in the middle of the probe"
	for d in 0.02 0.05 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 1.0 1.2 1.5; do
		emu_start; sleep $d; emu_stop; sleep 0.3
	done
	sleep 2
	card >/dev/null && fail "a device left behind" || pass "nothing left behind"
	ls /sys/bus/usb/drivers/fl2000 | grep -c ':1\.' | grep -q '^0$' && pass "no interface left behind" || fail "interfaces left"
	scan T60

	say "T61 the same thing, with the chip not answering"
	for d in 0.05 0.2 0.4 0.6; do
		emu_start --i2c-fail 100000; sleep $d; emu_stop; sleep 0.3
	done
	sleep 1
	scan T61

	say "T62 unbind and unplug during a storm of commits"
	emu_start; wait_card || fail "no device"
	U=$(ls /sys/bus/usb/drivers/fl2000 | grep ':1\.0$')
	for n in 1 2 3 4 5 6; do
		$T/fl2000-t storm $(card) 3 & S=$!
		sleep 1.$n
		echo $U > /sys/bus/usb/drivers/fl2000/unbind
		wait_nocard || fail "still bound"
		wait $S
		echo $U > /sys/bus/usb/drivers/fl2000/bind
		wait_card || fail "not bound again"
	done
	$T/fl2000-t storm $(card) 4 & S=$!
	sleep 2
	emu_stop
	wait_nocard || fail "still there after unplug"
	wait $S
	scan T62

	say "T63 fbdev open and written during the unplug"
	emu_start; wait_card || fail "no device"
	F=$(ls /dev/fb* | head -1)
	echo "fbdev: $F"
	( while dd if=/dev/urandom of=$F bs=64k count=64 2>/dev/null; do :; done ) & S=$!
	sleep 2
	emu_stop
	wait_nocard || fail "still there after unplug"
	sleep 0.5
	kill $S 2>/dev/null; wait $S 2>/dev/null
	sleep 1.5
	# A race in the core, see docs/DEVELOPMENT.md: the fbdev worker waits for a vblank as the
	# shutdown turns the CRTC off. Not what this test is looking for.
	dmesg | grep -q 'vblank wait timed out' && { echo "note: the known vblank wait warning of the fbdev worker"; dmesg -C; }
	scan T63

	say "T64 rmmod with the device open must refuse, then work"
	emu_start; wait_card || fail "no device"
	$T/fl2000-t hold $(card) 4 & S=$!
	sleep 1
	rmmod fl2000 2>/dev/null && fail "rmmod with an open file" || pass "rmmod refused with an open file"
	wait $S
	unload
	emu_stop; sleep 0.5; load
	scan T64

	say "T65 no EDID: DVI, standard modes"
	emu_start --edid 0; wait_card || fail "no device"
	[ "$(cat $(conn HDMI-A-1)/status)" = connected ] && pass "connected without EDID" || fail "status"
	dmesg | grep -E 'EDID read failed' | head -2
	HOLD=4 show_verify 1024 768 1024 768 0 0 xr24 3
	scan T65
	emu_stop; wait_nocard; sleep 0.3
}

fbcon() {
	say "T70 the console on the adapter, as when it is the only display"
	load
	emu_start; wait_card || fail "no device"
	sleep 3
	cat /sys/class/graphics/fb0/name
	f=$(frames); [ "${f:-0}" -gt 0 ] && pass "the console shows ($f frames)" || fail "no frames with fbcon"
	echo "hello from the console" > /dev/console
	sleep 1
	emu_stop
	wait_nocard && pass "device gone" || fail "device still there"
	sleep 1
	emu_start; wait_card && pass "console back on replug" || fail "no device on replug"
	sleep 2
	f=$(frames); [ "${f:-0}" -gt 0 ] && pass "frames after replug ($f)" || fail "no frames after replug"
	scan T70
	emu_stop; wait_nocard
	scan T70-unplug
}

race() {
	say "T71 monitor events, probes and commits all at once"
	load
	emu_start; wait_card || fail "no device"
	C=$(card); H=$(conn HDMI-A-1)/status; V=$(conn VGA-1)/status
	for tool in "flips $C 10" "storm $C 8"; do
		$T/fl2000-t ${tool%% *} ${tool#* } & F=$!
		sleep 1
		( for i in $(seq 1 16); do ctl hdmi 0; sleep 0.15; ctl vga 1; sleep 0.1; ctl vga 0; ctl hdmi 1; sleep 0.25; done ) & E=$!
		( while kill -0 $E 2>/dev/null; do echo detect > $H; echo detect > $V; cat $(dirname $H)/modes > /dev/null; sleep 0.05; done ) & P=$!
		wait $F; echo "${tool%% *} exit $?"
		wait $E; wait $P
	done
	ctl hdmi 1; sleep 1
	stats
	grep -c 'interrupt sent' /run/emu.log
	[ "$(detect HDMI-A-1)" = connected ] && pass "HDMI back at the end" || fail "HDMI lost"
	HOLD=4 show_verify 1024 768 1024 768 0 0 xr24 3
	scan T71
	emu_stop; wait_nocard
	scan T71-unplug
}

two() {
	say "T72 two adapters at once"
	load
	rm -f /run/ctl /run/emu.log; mkfifo /run/ctl /run/ctl2
	(exec 3<>/run/ctl; sleep 100000) & CTLH=$!
	(exec 3<>/run/ctl2; sleep 100000) & CTLH2=$!
	$T/fl2000-emu --ctl /run/ctl --log /run/emu.log & E1=$!
	$T/fl2000-emu --udc 1 --ctl /run/ctl2 --log /run/emu2.log & E2=$!
	sleep 5
	ls -d /sys/class/drm/card[0-9]
	n=$(ls -d /sys/class/drm/card[0-9] | wc -l); [ "$n" = 2 ] && pass "two DRM devices" || fail "$n DRM devices"
	ls /sys/bus/usb/drivers/fl2000 | grep -c ':1\.' | grep -q 6 && pass "six interfaces bound" || fail "interfaces: $(ls /sys/bus/usb/drivers/fl2000)"
	$T/fl2000-t show /dev/dri/card0 1024 768 1024 768 0 0 6 xr24 & S1=$!
	$T/fl2000-t show /dev/dri/card1 800 600 800 600 0 0 6 xr24 & S2=$!
	sleep 4
	echo stats > /run/ctl; echo stats > /run/ctl2; sleep 0.5
	grep frames= /run/emu.log | tail -1; grep frames= /run/emu2.log | tail -1
	echo dump /run/f1.bin > /run/ctl; echo dump /run/f2.bin > /run/ctl2; sleep 0.5
	python3 -I $T/verify_frame.py /run/f1.bin 1024 768 0 0 3 && pass "first adapter's frames" || fail "first adapter's frames"
	python3 -I $T/verify_frame.py /run/f2.bin 800 600 0 0 3 && pass "second adapter's frames" || fail "second adapter's frames"
	wait $S1; wait $S2
	$T/fl2000-t show /dev/dri/card1 800 600 800 600 0 0 6 xr24 & S2=$!
	sleep 3
	kill -9 $E1; sleep 1
	kill -0 $S2 && pass "the second keeps going when the first goes" || fail "the second stopped"
	f0=$(grep -c . /run/emu2.log); echo stats > /run/ctl2; sleep 0.5; grep frames= /run/emu2.log | tail -1
	wait $S2
	kill -9 $E2; sleep 1
	kill $CTLH $CTLH2
	scan T72
}

suspfail() {
	say "T67 saving the display state fails (patched module): the reset still works and nothing is left running"
	load
	emu_start; wait_card || fail "no device"
	U=$(ls /sys/bus/usb/drivers/fl2000 | grep ':1\.0$')
	$T/fl2000-t show $(card) 1024 768 1024 768 0 0 8 xr24 & S=$!
	sleep 3
	B=$(cat /sys/bus/usb/devices/${U%:*}/busnum); D=$(cat /sys/bus/usb/devices/${U%:*}/devnum)
	C=$(card)
	$T/fl2000-t reset $B $D && pass "USBDEVFS_RESET" || fail "USBDEVFS_RESET"
	sleep 1
	dmesg | grep -E 'Cannot save the display|resume error|Cannot restore' | head -3
	[ "$(card)" = "$C" ] && pass "same device" || fail "device changed"
	f0=$(frames); sleep 1; f1=$(frames)
	echo "frames after the failed reset: $f0 -> $f1 (the stream stays stopped until the next mode set)"
	wait $S
	HOLD=4 show_verify 1024 768 1024 768 0 0 xr24 3
	scan T67
	emu_stop; wait_nocard
	scan T67-unplug
}

leaks() {
	say "T50 leaks"
	emu_stop; sleep 1
	rmmod fl2000 2>/dev/null
	sleep 7
	echo scan > /sys/kernel/debug/kmemleak; sleep 7; echo scan > /sys/kernel/debug/kmemleak
	cat /sys/kernel/debug/kmemleak | grep -c unreferenced
	cat /sys/kernel/debug/kmemleak | head -60
}

case $SUITE in
all)	basic; hotplug; lifetime; faults; variants; stress; leaks;;
archall) basic; hotplug; lifetime; variants; stress;;
extra)	load; stress; emu_stop; load; emu_start; wait_card; faults; leaks;;
huge)	load; emu_start; sleep 6; ls /dev/dri; $T/fl2000-t hugemode /dev/dri/card0; echo "hugemode exit $?"; sleep 2; dmesg | grep -E 'KASAN|BUG:|Oops|general protection' | head -8; emu_stop;;
i2c0)	load; emu_start; wait_card; I=$(grep -l 'FL2000 bridge' /sys/bus/i2c/devices/i2c-*/name | head -1 | xargs dirname | xargs basename); $T/fl2000-t i2c0 /dev/$I; echo "i2c0 exit $?"; dmesg | grep -E 'KASAN|BUG:|Oops|general protection' | head -8;;
oldrmmod) load; emu_start; wait_card; rmmod fl2000; echo "rmmod exit $?"; sleep 1; emu_stop; sleep 2; dmesg | grep -E 'KASAN|BUG:|Oops|general protection|Unable to handle|unable to handle' | head -8;;
suspfail) suspfail;;
race) race;;
two) two;;
fbcon) fbcon;;
*)	$SUITE;;
esac

echo
say "RESULT: $fails failures"
sync
echo o > /proc/sysrq-trigger
sleep 30
