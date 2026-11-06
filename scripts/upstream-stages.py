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

final_objs = ['drm', 'drv', 'i2c', 'interrupt', 'it66121', 'registers', 'streaming', 'vga']
if mk != makefile(final_objs):
    sys.exit('drivers/gpu/drm/fl2000/Makefile changed: update makefile() and the stages')

# --- fl2000_drv.c, patches 2 and 3: IT66121 bridge, no DRM device ---
d2 = drv
d2 = cut(d2, '\tstruct fl2000_drm_if *drm_if;\n')
d2 = cut(d2, '\t/* The display was stopped for system sleep or a reset and has not been restarted yet */\n'
         '\tbool suspended;\n')
d2 = cut(d2, '\tdevs->drm_if = fl2000_drm_create(', '\tusb_set_intfdata(interface, devs);')
d2 = cut(d2, 'err_release_interfaces:\n', 'err_destroy_bridge:')
d2 = cut(d2, '\tfl2000_drm_destroy(devs->drm_if);\n')
# no DRM device to suspend yet: the callbacks only keep the driver bound across sleep
d2 = cut(d2, '/*\n * System sleep. The USB core suspends the interfaces one by one',
         'static struct usb_driver fl2000_driver = {',
         'static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)\n'
         '{\n'
         '\t/*\n'
         '\t * Nothing to save yet. Providing the callback keeps the USB core from\n'
         '\t * unbinding the driver across system sleep.\n'
         '\t */\n'
         '\tdev_dbg(&interface->dev, "suspend\\n");\n'
         '\n'
         '\treturn 0;\n'
         '}\n'
         '\n'
         'static int fl2000_resume(struct usb_interface *interface)\n'
         '{\n'
         '\tdev_dbg(&interface->dev, "resume\\n");\n'
         '\n'
         '\treturn 0;\n'
         '}\n'
         '\n')
d2 = cut(d2, '\t.reset_resume = fl2000_resume,\n')
d2 = cut(d2, '\t.pre_reset = fl2000_pre_reset,\n')
d2 = cut(d2, '\t.post_reset = fl2000_resume,\n')

# --- fl2000_drv.c, patch 1: no bridge either ---
d1 = d2
d1 = cut(d1, '#include "fl2000_it66121.h"\n')
d1 = cut(d1, '\tstruct drm_bridge *bridge;\n')
d1 = cut(d1, '\t/* The IT66121 is looked for with the I2C engine', '\tret = fl2000_claim_interfaces(')
d1 = cut(d1, '\tif (ret)\n\t\tgoto err_destroy_bridge;\n', None, '\tif (ret)\n\t\treturn ret;\n')
d1 = cut(d1, '\nerr_destroy_bridge:\n', '}\n\nstatic void fl2000_disconnect(')
d1 = cut(d1, '\tit66121_destroy(devs->bridge);\n\n')

# --- fl2000.h, patch 3: no interrupt or DRM device yet ---
h3 = hdr
h3 = cut(h3, '/* Interrupt polling task */', '/* I2C adapter interface creation */')
h3 = cut(h3, '/* DRM device creation */', '#endif /* FL2000_H */')

# --- fl2000.h, patches 1 and 2: no streaming either ---
h1 = h3
h1 = cut(h1, '/* Called from URB completion handlers, i.e. in atomic context */',
         'struct fl2000_timings {')
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
