# gstreamer-rgaconvert
video size colorspace convert for rockchip rga hardware

## Installation

Build a Debian package and install it:

```sh
./packaging/build-deb.sh
sudo apt install ./packaging/gstreamer1.0-rgaconvert_*.deb
```

This installs `libgstrgaconvert.so` into the system GStreamer plugin
directory (`/usr/lib/<triplet>/gstreamer-1.0/`), so pipelines find the
`rgaconvert` element without setting `GST_PLUGIN_PATH`. Verify with:

```sh
gst-inspect-1.0 rgaconvert
```

For development, build with meson and point GStreamer at the build dir
instead:

```sh
meson setup build && meson compile -C build
GST_PLUGIN_PATH=$PWD/build/plugins gst-inspect-1.0 rgaconvert
```
