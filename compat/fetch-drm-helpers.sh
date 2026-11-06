#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Fetch, unchanged, DRM display helpers that a distribution kernel was built
# without, so that the out-of-tree build can compile them into fl2000.ko (the
# top-level Makefile runs this when it needs them). They come from the
# kernel's stable tree, at the tag of the target kernel's release:
#
#   compat/fetch-drm-helpers.sh <kernel release> <directory> <helper>...
#   compat/fetch-drm-helpers.sh 7.2.11-arch1-1 compat/drm-helpers/7.2 drm_bridge_connector
#
# Commit the directory it fills: later builds for the same kernel series then
# work offline, and the Makefile checks the files against SHA256SUMS.

set -eu

if [ $# -lt 3 ]; then
	echo "usage: $0 <kernel release> <directory> <helper>..." >&2
	exit 2
fi
release=$1
dir=$2
shift 2

# 7.2.11-arch1-1 -> v7.2.11, 7.3.0-arch1-1 -> v7.3, 7.4.0-rc1-1-mainline -> v7.4-rc1
majmin=$(printf '%s\n' "$release" | sed -nE 's/^([0-9]+\.[0-9]+).*/\1/p')
patch=$(printf '%s\n' "$release" | sed -nE 's/^[0-9]+\.[0-9]+\.([0-9]+).*/\1/p')
rc=$(printf '%s\n' "$release" | sed -nE 's/^[0-9]+\.[0-9]+(\.[0-9]+)?-(rc[0-9]+).*/\2/p')
if [ -z "$majmin" ]; then
	echo "$0: cannot tell the version of kernel release '$release'" >&2
	exit 1
fi
if [ -n "$rc" ]; then
	tag="v$majmin-$rc"
elif [ -n "$patch" ] && [ "$patch" != 0 ]; then
	tag="v$majmin.$patch"
else
	tag="v$majmin"
fi

# GitHub mirror of the stable tree, which also carries the mainline tags
# (git.kernel.org refuses automated downloads)
base="https://raw.githubusercontent.com/gregkh/linux/$tag/drivers/gpu/drm/display"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for helper in "$@"; do
	echo "  FETCH   $helper.c ($tag)"
	if ! curl -fsSL --retry 2 -o "$tmp/$helper.c" "$base/$helper.c"; then
		echo "$0: cannot download $base/$helper.c" >&2
		echo "Build against a kernel that has the DRM display helpers, or copy" >&2
		echo "drivers/gpu/drm/display/$helper.c of Linux $tag into $dir by hand." >&2
		exit 1
	fi
	if ! head -n 1 "$tmp/$helper.c" | grep -q 'SPDX-License-Identifier'; then
		echo "$0: $base/$helper.c is not kernel source" >&2
		exit 1
	fi
done

mkdir -p "$dir"
mv "$tmp"/*.c "$dir"/
(cd "$dir" && sha256sum -- *.c > SHA256SUMS)
cat > "$dir/SOURCE" <<EOF
Unmodified copies of drivers/gpu/drm/display/*.c from Linux $tag,
fetched from $base
by compat/fetch-drm-helpers.sh for kernel release $release.
Compiled into fl2000.ko only for kernels built without them; never part of the
upstream patches.
EOF
