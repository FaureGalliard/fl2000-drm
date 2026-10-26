#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Regenerate docs/upstream/stages/*.diff: the reduced versions of
# fl2000_drv.c, fl2000.h and the Makefile used by patches 1-3 of the
# upstream series (see scripts/make-upstream-series.sh). Each stage is
# derived from the final sources by cutting the code that a later patch
# adds, between textual anchors; if an anchor disappears this script fails
# and the cut below needs updating.

import difflib
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRV = os.path.join(REPO, 'drivers/gpu/drm/fl2000')
OUT = os.path.join(REPO, 'docs/upstream/stages')


def cut(s, start, end=None, replace=''):
    """Remove s[start:end) (end excluded), or just the string start."""
    i = s.find(start)
    if i < 0:
        sys.exit('anchor not found: %r' % start[:60])
    if end is None:
        return s[:i] + replace + s[i + len(start):]
    j = s.find(end, i)
    if j < 0:
        sys.exit('anchor not found: %r' % end[:60])
    return s[:i] + replace + s[j:]


def read(name):
    with open(os.path.join(DRV, name)) as f:
        return f.read()


def makefile(objs):
    return ('# SPDX-License-Identifier: GPL-2.0-only\n\nfl2000-y := \\\n' +
            ' \\\n'.join('\tfl2000_%s.o' % o for o in objs) +
            '\n\nobj-$(CONFIG_DRM_FL2000) += fl2000.o\n')


drv = read('fl2000_drv.c')
hdr = read('fl2000.h')
mk = read('Makefile')

final_objs = ['drm', 'drv', 'i2c', 'interrupt', 'it66121', 'registers', 'streaming']
if mk != makefile(final_objs):
    sys.exit('drivers/gpu/drm/fl2000/Makefile changed: update makefile() and the stages')

# --- fl2000_drv.c, patches 2 and 3: IT66121 bridge, no component master ---
d2 = drv
d2 = cut(d2, '\tstruct component_match *match;\n')
d2 = cut(d2, 'static const struct component_master_ops fl2000_master_ops = {',
         'static int fl2000_compare(')
d2 = cut(d2, 'static int fl2000_compare(', 'static struct fl2000_devs *fl2000_get_devices(')
d2 = cut(d2, '\tcomponent_match_add(', '\tdev_set_drvdata(&usb_dev->dev, devs);')
d2 = cut(d2, '\n\t/* When all interfaces are up - proceed with registration */',
         '\n\treturn ret;\n}\n\nstatic void fl2000_disconnect(')
d2 = cut(d2, '\t/*\n\t * The IT66121 bridge component is intentionally NOT destroyed here.',
         '\tswitch (iface_num) {',
         '\t/*\n'
         '\t * The IT66121 bridge is intentionally NOT destroyed here: disconnect\n'
         '\t * also runs for the transient unbind/rebind cycles of\n'
         '\t * usb_reset_device(), where the usb_device survives. Its destruction is\n'
         '\t * a devres of the usb_device, see fl2000_bridge_create().\n'
         '\t */\n')
d2 = cut(d2, '#define FL2000_ALL_IFS \\', 'static const struct usb_device_id')

# --- fl2000_drv.c, patch 1: no bridge either ---
d1 = d2
d1 = cut(d1, '#include "fl2000_it66121.h"\n')
d1 = cut(d1, '\tstruct i2c_client *bridge_client;\n')
d1 = cut(d1, 'static void fl2000_bridge_release(', 'static int fl2000_probe(')
d1 = cut(d1, '\t/*\n\t * Create the IT66121 bridge if it does not exist yet.',
         '\tswitch (iface_num) {')
d1 = cut(d1, '\t/*\n\t * The IT66121 bridge is intentionally NOT destroyed here',
         '\tswitch (iface_num) {')

# --- fl2000.h, patch 3: no interrupt or DRM device yet ---
h3 = hdr
h3 = cut(h3, '/* Interrupt polling task */', '/* I2C adapter interface creation */')
h3 = cut(h3, '/* DRM device creation */', '#endif /* FL2000_H */')

# --- fl2000.h, patches 1 and 2: no streaming either ---
h1 = h3
h1 = cut(h1, '/* Process context only: may sleep between retries */', 'struct fl2000_timings {')
h1 = cut(h1, '/* Streaming transfer task */', '/* I2C adapter interface creation */')

stages = {
    1: {'fl2000_drv.c': d1, 'fl2000.h': h1, 'Makefile': makefile(['drv', 'i2c', 'registers'])},
    2: {'fl2000_drv.c': d2, 'fl2000.h': h1,
        'Makefile': makefile(['drv', 'i2c', 'it66121', 'registers'])},
    3: {'fl2000_drv.c': d2, 'fl2000.h': h3,
        'Makefile': makefile(['drv', 'i2c', 'it66121', 'registers', 'streaming'])},
}

os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    if f.endswith('.diff'):
        os.remove(os.path.join(OUT, f))

for n, files in stages.items():
    for name, content in files.items():
        final = read(name)
        if content == final:
            continue
        diff = difflib.unified_diff(final.splitlines(True), content.splitlines(True),
                                    'a/' + name, 'b/' + name)
        with open(os.path.join(OUT, '%d-%s.diff' % (n, name)), 'w') as f:
            f.writelines(diff)

print('stages written to', os.path.relpath(OUT, REPO))
