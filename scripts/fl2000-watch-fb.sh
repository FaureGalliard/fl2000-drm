#!/bin/bash
# Print, once a second, which framebuffer the compositor is scanning out on
# the fl2000 primary plane, and whether it is the primary GPU's buffer
# imported as is (zero-copy) or a dumb buffer of ours that the compositor
# filled with a CPU copy. Needs root for debugfs.
#
#   sudo bash scripts/fl2000-watch-fb.sh      (Ctrl+C to stop)

set -u

card=$(for d in /sys/kernel/debug/dri/*/name; do
	grep -q '^fl2000 ' "$d" 2>/dev/null && basename "$(dirname "$d")"
done | head -1)
if [ -z "$card" ]; then
	echo "no fl2000 device in /sys/kernel/debug/dri (run as root)"
	exit 1
fi

while true; do
	# First plane listed is the primary one; column 3 is its framebuffer id
	fb=$(modetest -M fl2000 -p 2>/dev/null |
	     awk '/^Planes:/ { p = 1; next } p && /^[0-9]+\t/ { print $3; exit }')
	info=$(awk -v want="framebuffer[$fb]:" '
		$1 == want { p = 1; next }
		p && /^framebuffer\[/ { exit }
		p && /allocated by/ { sub(/.*= /, ""); who = $0 }
		p && /format=/ { sub(/.*format=/, ""); fmt = $1 }
		p && /size=[0-9]+x/ && !size { sub(/.*size=/, ""); size = $1 }
		p && /pitch\[0\]/ { sub(/.*=/, ""); pitch = $1 }
		p && /imported=/ { sub(/.*=/, ""); imp = $1 }
		END { printf "%s %s pitch=%s imported=%s (%s)", fmt, size, pitch, imp, who }
	' "/sys/kernel/debug/dri/$card/framebuffer")
	echo "$(date +%T) fb=$fb $info"
	sleep 1
done
