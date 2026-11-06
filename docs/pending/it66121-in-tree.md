# Reusing the in-tree ite-it66121 bridge: what it would take

Phase 1 of step 3 (5 November 2026): read `drivers/gpu/drm/bridge/ite-it66121.c`
of v7.2.11 and of drm-misc-next (7.4-rc1, which differs only in the
`drm_atomic_commit` rename) against what an FL2000 adapter on a PC can
provide. Nothing was changed or tested on hardware yet.

## Blockers, in order of size

1. **Device tree only, and PCs have none.** `DRM_ITE_IT66121` has
   `depends on OF`; Arch's x86 kernel has `# CONFIG_OF is not set`, like most
   PC distributions. `it66121_probe()` reads everything through OF:
   - `of_graph_get_endpoint_by_regs(dev->of_node, 0, 0)` for `bus-width`
     (12 or 24) and returns `-EINVAL` without it;
   - `of_graph_get_remote_node(dev->of_node, 1, -1)` and
     `of_drm_find_and_get_bridge()` for the *next* bridge, `-EINVAL` if
     unconnected and `-EPROBE_DEFER` until it exists;
   - `of_property_present(dev->of_node, "#sound-dai-cells")` for audio.
   A software node (`i2c_board_info.swnode`) can carry the same graph and
   properties, but the driver would have to read them with the `fwnode_*` /
   `fwnode_graph_*` API instead.
2. **DRM has no bridge lookup without OF.** `drm_bridge.h` (7.2 and 7.4)
   only offers `of_drm_find_bridge()`, `of_drm_find_and_get_bridge()` and
   `of_drm_get_bridge_by_endpoint()`, stubbed to NULL without `CONFIG_OF`.
   Nothing in `drm_bridge.c` or `drivers/gpu/drm/bridge/` uses fwnode or
   software nodes. So neither the IT66121 can find its next bridge, nor the
   FL2000 driver the IT66121 bridge, without a new core helper (lookup by
   fwnode) or another way to hand the bridge over.
3. **Mandatory reset GPIO.** `devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW)`
   fails without one; on the FL2000 boards there is no GPIO the host controls
   (would become `devm_gpiod_get_optional()`). The three supplies are fine:
   absent ones become dummy regulators.
4. **EDID through the IT66121 DDC engine**, which never completes on this
   board (the monitor's DDC hangs off the FL2000 I2C bus). This one needs no
   change: with `drm_bridge_connector` the last bridge with
   `DRM_BRIDGE_OP_EDID` wins, so a small next bridge of ours reading the EDID
   with the FL2000 engine would take over, and `DRM_BRIDGE_OP_DETECT` would
   stay with the IT66121. The IRQ is already optional (`client->irq > 0`):
   without one the connector polls, as now.
5. **I2C transfers.** The driver uses `regmap_bulk_write()` for infoframes
   and `regmap_noinc_read()` for the EDID FIFO; the FL2000 adapter takes one
   data byte per transfer. regmap-i2c should split them from the adapter
   quirks, to be checked on hardware.

## What reusing it would mean upstream

Three series instead of one, to three sets of maintainers: a DRM core helper
(bridge lookup by fwnode), ite-it66121 converted from OF to fwnode with an
optional reset and without `depends on OF` (bridge maintainers, and every
board that uses it today must keep working), then this driver. klogg's fork
shows the hardware side can be made to work, but he rewrote the driver into a
local variant ("not interchangeable" with the in-tree module) and creates the
I2C client from a udev service, which is not an upstream path.

Decision of 5 Nov 2026: do not build this blind. The RFC asks the
maintainers (cover letter, question 1) with these facts; their answer picks
between keeping `fl2000_it66121.c` and the work above.
