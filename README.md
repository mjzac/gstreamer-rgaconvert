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

**Build on the oldest distro you deploy to.** GStreamer refuses plugins
compiled against a newer GStreamer than the runtime (a 1.24-built plugin
is blacklisted on a 1.20 system; the reverse works fine). The package
declares the compiled-against version as a dependency, so installing a
too-new build fails at `apt install` time with a clear error.

```sh
gst-inspect-1.0 rgaconvert
```

For development, build with meson and point GStreamer at the build dir
instead:

```sh
meson setup build && meson compile -C build
GST_PLUGIN_PATH=$PWD/build/plugins gst-inspect-1.0 rgaconvert
```
