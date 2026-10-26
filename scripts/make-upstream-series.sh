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

# --- driver sources -------------------------------------------------------
rm -rf "$DRV"
mkdir -p "$DRV"
cp "$REPO/$DRV"/Kconfig "$REPO/$DRV"/Makefile "$REPO/$DRV"/*.[ch] "$DRV"/

# --- Kconfig (alphabetical) and Makefile (next to udl) hooks --------------
grep -q 'drm/fl2000/Kconfig' drivers/gpu/drm/Kconfig ||
	sed -i '/^source "drivers\/gpu\/drm\/fsl-dcu\/Kconfig"/i source "drivers/gpu/drm/fl2000/Kconfig"' \
		drivers/gpu/drm/Kconfig
grep -q 'CONFIG_DRM_FL2000' drivers/gpu/drm/Makefile ||
	sed -i '/^obj-\$(CONFIG_DRM_UDL)/i obj-$(CONFIG_DRM_FL2000) += fl2000/' \
		drivers/gpu/drm/Makefile
grep -q 'drm/fl2000/Kconfig' drivers/gpu/drm/Kconfig
grep -q 'CONFIG_DRM_FL2000' drivers/gpu/drm/Makefile

# --- MAINTAINERS, kept in alphabetical order ------------------------------
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

git add "$DRV" drivers/gpu/drm/Kconfig drivers/gpu/drm/Makefile MAINTAINERS
git commit -q -s -F "$REPO/docs/upstream/commit-message.txt"

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
./scripts/get_maintainer.pl --no-rolestats "$OUT"/*0001-*.patch || true
