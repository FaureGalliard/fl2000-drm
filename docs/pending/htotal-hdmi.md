# Pending: the htotal adjustment for HDMI

`fl2000_mode_calc()` can stretch or shrink htotal by up to 10 pixels (keeping
the refresh rate) until the pixel clock lands on a setting the PLL can make. The
loop never did it (the sign started at 0); that is fixed, and the VGA output
uses it. HDMI is deliberately left out: `fl2000_hdmi_encoder_mode_valid()` in
`fl2000_drm.c` only accepts the modes the PLL hits with the standard timings,
which is exactly what the driver offered before the fix.

What allowing it on HDMI would change, simulated with the driver's own PLL code:

- 1920x1080@60.00 (148.5 MHz) becomes valid, programmed with htotal 2198 instead
  of 2200. The monitor's preferred mode is usually that one, so the desktop would
  switch from the 59.94 Hz mode verified on hardware to it.
- 1280x1024@60, 1366x768@60, 1440x900@60, 1600x900@60, 1920x1080@50,
  1920x1200@60 (reduced blanking), 2560x1440@30 and 3840x2160@30 become
  available, each with htotal changed by at most 10 pixels.

To enable it, once an HDMI monitor has shown the adjusted 1080p60 mode without
problems: remove `fl2000_hdmi_encoder_mode_valid()` and its helper table, and
the `drm_encoder_helper_add()` call for the HDMI encoder.
