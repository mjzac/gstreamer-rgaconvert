# gstreamer-rgaconvert

A GStreamer video converter element (`rgaconvert`) for Rockchip SoCs that
scales, colour-converts, rotates and flips raw video on the RGA 2D hardware
accelerator (via the librga im2d API) — all in a single hardware blit, with
zero-copy dma-buf input and output.

## Capabilities

- **Scaling** — driven by caps negotiation, no properties needed: whatever
  resolution downstream requests (or a capsfilter forces) is what RGA scales
  to. Input up to 8192x8192, output up to 4096x4096. No aspect-ratio
  preservation: you get exactly the rectangle you ask for.
- **Colour conversion** — between `I420, YV12, NV12, NV21, Y42B, NV16, NV61,
  RGB16, RGB15, BGR, RGB, BGRA, RGBA, BGRx, RGBx`.
- **Rotation and flipping** — through the standard
  [`video-direction`](https://gstreamer.freedesktop.org/documentation/video/gstvideodirection.html)
  property (the `GstVideoDirection` interface, same as `videoflip`), so
  `rgaconvert` is a hardware drop-in replacement for `videoflip`:

  | value | effect |
  |---|---|
  | `identity` | no rotation (default) |
  | `90r` / `180` / `90l` | rotate 90° clockwise / 180° / 90° counter-clockwise |
  | `horiz` / `vert` | mirror horizontally / vertically |
  | `ul-lr` / `ur-ll` | flip across the upper-left or upper-right diagonal |
  | `auto` | follow the stream's `image-orientation` tag (EXIF etc.) |

  90°/270° and diagonal methods automatically negotiate a width/height-swapped
  output size, and changing the property at runtime renegotiates mid-stream.
- **Zero-copy** — both pads advertise the `memory:DMABuf` caps feature.
  dma-buf producers (`mppvideodec`, V4L2 sources in dmabuf io-mode, ...)
  are imported directly into RGA, and output is written into a
  dma-heap-backed dma-buf pool that downstream (e.g. `kmssink`) can scan out
  without a copy. When no dma-buf is available on either side it falls back
  to CPU-mapped memory transparently.

All of the above happen in one `improcess()` blit per frame.

## Examples

Scale and convert 1080p NV12 to 640x480 RGBA:

```sh
gst-launch-1.0 videotestsrc ! video/x-raw,format=NV12,width=1920,height=1080 \
    ! rgaconvert ! video/x-raw,format=RGBA,width=640,height=480 ! autovideosink
```

Rotate 90° clockwise (output size 720x1280 is negotiated automatically):

```sh
gst-launch-1.0 videotestsrc ! video/x-raw,format=NV12,width=1280,height=720 \
    ! rgaconvert video-direction=90r ! autovideosink
```

Scale, convert and rotate in a single blit:

```sh
gst-launch-1.0 videotestsrc ! video/x-raw,format=NV12,width=1920,height=1080 \
    ! rgaconvert video-direction=90r ! video/x-raw,format=RGBA,width=540,height=960 \
    ! autovideosink
```

Follow the stream's orientation metadata:

```sh
gst-launch-1.0 videotestsrc ! taginject tags="image-orientation=rotate-90" \
    ! rgaconvert video-direction=auto ! autovideosink
```

## Device permissions

RGA and the dma-heap device nodes (`/dev/rga`, `/dev/dma_heap/*`) are usually
owned by `root:video`. Add your user to the `video` group (or run the
pipeline as root), otherwise frame import fails with
`failed to open RGA: Permission denied`.

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
