#!/bin/bash
# Quick post-reboot health check of the fl2000 driver. Needs no root: it only
# reads the journal, sysfs and lsusb. Usage: scripts/post-boot-check.sh [logfile]

out=${1:-/dev/stdout}

{
echo "=== boot: $(uptime -s), kernel $(uname -r)"
echo "=== installed module"
modinfo -F srcversion /lib/modules/"$(uname -r)"/extra/fl2000.ko 2>&1
cat /sys/module/fl2000/srcversion 2>/dev/null || echo "fl2000 NOT loaded"
for p in /sys/module/fl2000/parameters/*; do
	[ -e "$p" ] && echo "  $(basename "$p")=$(cat "$p")"
done

echo "=== USB"
lsusb -d 1d5c:2000 || echo "FL2000 not on the bus"

echo "=== DRM connectors"
for c in /sys/class/drm/card*-*; do
	drv=$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)" 2>/dev/null)
	echo "  $(basename "$c"): $(cat "$c/status" 2>/dev/null) $(cat "$c/enabled" 2>/dev/null) modes=$(head -1 "$c/modes" 2>/dev/null) [$drv]"
done

echo "=== previous boot: last kernel lines (empty tail = hard lockup)"
journalctl -k -b -1 --no-pager 2>/dev/null | tail -5

echo "=== this boot: fl2000 / it66121 / usb 1d5c"
journalctl -k -b --no-pager | grep -iE "fl2000|it66121|1d5c|\bdrm\b.*fl2000" | grep -v "Stream:" | head -80

echo "=== this boot: errors and warnings mentioning the driver or USB"
journalctl -k -b -p warning --no-pager | grep -iE "fl2000|it66121|usb 2-|xhci|WARNING|BUG|Oops|Call Trace|lockdep" | head -40

echo "=== fence fallback (IRQ starvation symptom)"
journalctl -k -b --no-pager | grep -ci "fence fallback"

echo "=== amdgpu bind failed"
journalctl -k -b --no-pager | grep -ci "bind failed"

echo "=== Stream lines (last 15)"
journalctl -k -b --no-pager | grep "Stream:" | tail -15

echo "=== mutter on secondary GPU"
journalctl -b --user --no-pager 2>/dev/null | grep -iE "fl2000|secondary GPU|Not hardware accelerated|udmabuf|card[0-9].*fail" | tail -15
} >"$out" 2>&1
