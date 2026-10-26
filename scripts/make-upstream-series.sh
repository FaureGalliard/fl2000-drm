#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Turn this repository into a patch series for the Linux kernel.
#
# Usage: scripts/make-upstream-series.sh <linux-tree> [output-dir]
#
# <linux-tree> must be a git checkout of the tree the patches will be
# applied to - for a DRM driver that is drm-misc-next:
#
#   git clone https://gitlab.freedesktop.org/drm/misc/kernel.git -b drm-misc-next
#
# The script creates a branch "fl2000-upstream" in that tree, commits the
# driver on top of the current HEAD, and writes the series (cover letter
# included) to [output-dir] (default: ./outgoing). Nothing is sent: see
# docs/UPSTREAMING.md for the review and sending steps.
#
# The driver is split into a series of patches that each build on their
# own; docs/upstream/series/<n>.txt are their commit messages and
# docs/upstream/stages/ holds the reduced intermediate versions of the
# files that grow over the series. Set CHECK_BUILD=1 (in an already
# configured tree with CONFIG_DRM_FL2000=m) to build-test every patch.
#
# Patch identity comes from git config user.name / user.email in the
# linux tree. The patch carries a Signed-off-by for that identity: adding
# it certifies the Developer's Certificate of Origin
# (Documentation/process/submitting-patches.rst), so only run this as the
# person who is going to send the series.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LINUX="${1:?usage: $0 <linux-tree> [output-dir]}"
OUT="$(realpath -m "${2:-$REPO/outgoing}")"
DRV=drivers/gpu/drm/fl2000
BRANCH=fl2000-upstream
VERSION="${VERSION:-1}"
PREFIX="${PREFIX:-RFC PATCH}"

cd "$LINUX"
git rev-parse --git-dir >/dev/null

NAME="$(git config user.name)"
EMAIL="$(git config user.email)"
if [ -z "$NAME" ] || [ -z "$EMAIL" ]; then
	echo "Set git config user.name and user.email in $LINUX first" >&2
	exit 1
fi

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	echo "$LINUX has uncommitted changes" >&2
	exit 1
fi

BASE="$(git rev-parse HEAD)"
git checkout -q -B "$BRANCH" "$BASE"

STAGES="$REPO/docs/upstream/stages"
SERIES="$REPO/docs/upstream/series"

# Copy final versions of the given files into the tree, then turn the files
# that only exist in reduced form at this point of the series into that form
# (docs/upstream/stages/<n>-<file>.diff, applied to the final version).
stage() {
	local n=$1 f
	shift
	for f in "$@"; do
		cp "$REPO/$DRV/$f" "$DRV/$f"
	done
	for f in "$STAGES/$n"-*.diff; do
		[ -e "$f" ] || continue
		cp "$REPO/$DRV/$(basename "$f" .diff | cut -d- -f2-)" "$DRV/"
		patch -s -p1 -d "$DRV" < "$f"
	done
}

commit_stage() {
	local n=$1
	shift
	git add "$DRV" "$@"
	git commit -q -s -F "$SERIES/$n.txt"
	if [ -n "${CHECK_BUILD:-}" ]; then
		# Every patch must build on its own (bisectability)
		make -s W=1 "$DRV/"
		echo "patch $n builds"
	fi
}

rm -rf "$DRV"
mkdir -p "$DRV"

# --- 1: USB driver, registers, I2C, Kbuild and MAINTAINERS hooks ---------
stage 1 Kconfig Makefile fl2000.h fl2000_drv.c fl2000_registers.h fl2000_registers.c \
	fl2000_i2c.c

# Kconfig (alphabetical) and Makefile (next to udl) hooks
grep -q 'drm/fl2000/Kconfig' drivers/gpu/drm/Kconfig ||
	sed -i '/^source "drivers\/gpu\/drm\/fsl-dcu\/Kconfig"/i source "drivers/gpu/drm/fl2000/Kconfig"' \
		drivers/gpu/drm/Kconfig
grep -q 'CONFIG_DRM_FL2000' drivers/gpu/drm/Makefile ||
	sed -i '/^obj-\$(CONFIG_DRM_UDL)/i obj-$(CONFIG_DRM_FL2000) += fl2000/' \
		drivers/gpu/drm/Makefile
grep -q 'drm/fl2000/Kconfig' drivers/gpu/drm/Kconfig
grep -q 'CONFIG_DRM_FL2000' drivers/gpu/drm/Makefile

# MAINTAINERS, kept in alphabetical order
if ! grep -q '^F:	drivers/gpu/drm/fl2000/' MAINTAINERS; then
	ENTRY="DRM DRIVER FOR FRESCO LOGIC FL2000 USB DISPLAY ADAPTERS
M:	$NAME <$EMAIL>
L:	dri-devel@lists.freedesktop.org
S:	Maintained
T:	git https://gitlab.freedesktop.org/drm/misc/kernel.git
F:	drivers/gpu/drm/fl2000/
"
	ENTRY="$ENTRY" perl -0pi -e '
		my $e = $ENV{ENTRY};
		s/^(DRM DRIVER FOR GENERIC EDP PANELS\n)/$e\n$1/m or die "MAINTAINERS anchor not found\n";
	' MAINTAINERS
fi
commit_stage 1 drivers/gpu/drm/Kconfig drivers/gpu/drm/Makefile MAINTAINERS

# --- 2: IT66121 bridge ----------------------------------------------------
stage 2 fl2000_it66121.h fl2000_it66121_regs.h fl2000_it66121.c
commit_stage 2

# --- 3: frame streaming ---------------------------------------------------
stage 3 fl2000_streaming.c
commit_stage 3

# --- 4: KMS pipeline: the final driver ------------------------------------
stage 4 Kconfig Makefile fl2000.h fl2000_drv.c fl2000_drm.c fl2000_interrupt.c
commit_stage 4

# The end of the series must be exactly the driver in this repository
diff -r "$REPO/$DRV" "$DRV" -x '*.o' -x '*.ko' -x '*.mod*' -x '.*' -x modules.order \
	-x Module.symvers >/dev/null ||
	{ echo "series does not end with the repository's driver" >&2; exit 1; }

# --- series ---------------------------------------------------------------
rm -rf "$OUT"
VERSION_ARG=()
[ "$VERSION" -gt 1 ] && VERSION_ARG=(-v "$VERSION")
git format-patch -q --cover-letter --base="$BASE" "${VERSION_ARG[@]}" \
	--subject-prefix="$PREFIX" -o "$OUT" "$BASE"..HEAD

# Fill the cover letter from docs/upstream/cover-letter.txt: first line is
# the subject, the rest replaces the *** BLURB HERE *** placeholder.
COVER="$(ls "$OUT"/*0000-cover-letter.patch)"
SUBJECT="$(head -n1 "$REPO/docs/upstream/cover-letter.txt")"
BLURB="$(tail -n +3 "$REPO/docs/upstream/cover-letter.txt")
$NAME"
SUBJECT="$SUBJECT" BLURB="$BLURB" perl -0pi -e '
	s/\*\*\* SUBJECT HERE \*\*\*/$ENV{SUBJECT}/;
	s/\*\*\* BLURB HERE \*\*\*/$ENV{BLURB}/;
' "$COVER"

echo "Series written to $OUT (base $BASE, branch $BRANCH in $LINUX)"
echo
echo "== checkpatch =="
./scripts/checkpatch.pl --strict "$OUT"/*.patch || true
echo
echo "== get_maintainer (To/Cc candidates) =="
./scripts/get_maintainer.pl --no-rolestats "$OUT"/*000[1-9]-*.patch || true
