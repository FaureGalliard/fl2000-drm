# Pending: 1920x1080@60.00 on HDMI

HDMI only offers the modes whose pixel clock the PLL hits within 500 ppm
(`FL2000_PPM_ERR_MAX`), exactly what the driver offered before the VGA work. The
VGA output accepts 5000 ppm (`FL2000_PPM_ERR_MAX_VGA`).

That leaves out 1920x1080@60.00 (148.5 MHz) on HDMI: the PLL makes 148.333 MHz
for it, 1122 ppm off, so the monitor's preferred 60.00 Hz mode is rejected and
the 59.94 Hz one (148.352 MHz, which the PLL also turns into 148.333 MHz) is used.
Accepting the larger error would program the very same registers as that
verified mode, with the standard htotal 2200, and make the 60.00 Hz mode valid.
It would also add 1280x1024@60 (2057 ppm) and a few more.

Never fix this by stretching htotal: on the analog output it made a monitor
show nothing or half a picture, and a digital sink expects the exact timing too.

To try it with an HDMI monitor at hand: lower the strictness in
`fl2000_hdmi_encoder_mode_valid()` (or remove it and its helper table and the
`drm_encoder_helper_add()` call for the HDMI encoder) and check that the monitor
still shows a stable picture.
