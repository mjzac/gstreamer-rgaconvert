#!/usr/bin/env bash
#
# Build a Debian package for the rgaconvert GStreamer plugin.
#
# Usage:
#   ./packaging/build-deb.sh
#
# Requires only meson, ninja and dpkg-deb (no debhelper). The result is
# packaging/gstreamer1.0-rgaconvert_<version>_<arch>.deb; install it with:
#   sudo apt install ./packaging/gstreamer1.0-rgaconvert_<version>_<arch>.deb
#
# Override metadata via environment if needed:
#   MAINTAINER="Name <email>" ./packaging/build-deb.sh
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC_DIR=$(dirname "$SCRIPT_DIR")
BUILD_DIR="$SCRIPT_DIR/build"
STAGING="$BUILD_DIR/staging"

PACKAGE=gstreamer1.0-rgaconvert
ARCH=$(dpkg --print-architecture)
TRIPLET=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
MAINTAINER=${MAINTAINER:-"$(git -C "$SRC_DIR" config user.name 2>/dev/null || echo unknown) <$(git -C "$SRC_DIR" config user.email 2>/dev/null || echo unknown@localhost)>"}

# Fresh release build, separate from the dev build/ directory.
meson setup "$BUILD_DIR" "$SRC_DIR" \
    --prefix=/usr \
    --libdir="lib/$TRIPLET" \
    --buildtype=release \
    --wipe
meson compile -C "$BUILD_DIR"

VERSION=$(meson introspect "$BUILD_DIR" --projectinfo | sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' | head -n1)
if [ -z "$VERSION" ]; then
    echo "error: could not determine project version" >&2
    exit 1
fi

rm -rf "$STAGING"
meson install -C "$BUILD_DIR" --destdir "$STAGING"

# The generated pkg-config file is sample-project boilerplate; the package
# ships only the plugin.
rm -f "$STAGING/usr/lib/$TRIPLET/pkgconfig/gst-rga_convert.pc"
rmdir --ignore-fail-on-non-empty "$STAGING/usr/lib/$TRIPLET/pkgconfig" 2>/dev/null || true

install -d -m 755 "$STAGING/usr/share/doc/$PACKAGE"
install -m 644 "$SRC_DIR/LICENSE" "$STAGING/usr/share/doc/$PACKAGE/copyright"

INSTALLED_SIZE=$(du -sk --exclude=DEBIAN "$STAGING" | cut -f1)

install -d -m 755 "$STAGING/DEBIAN"
cat > "$STAGING/DEBIAN/control" <<EOF
Package: $PACKAGE
Version: $VERSION
Section: libs
Priority: optional
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $INSTALLED_SIZE
Depends: librga2, libgstreamer1.0-0 (>= 1.16), libgstreamer-plugins-base1.0-0 (>= 1.16), libc6
Description: GStreamer plugin for Rockchip RGA video conversion
 Provides the rgaconvert element, which uses the Rockchip RGA 2D engine
 for hardware-accelerated video format conversion, scaling, rotation and
 flipping, with zero-copy dma-buf import/export.
EOF

DEB="$SCRIPT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGING" "$DEB"

echo
echo "Built: $DEB"
echo "Install with: sudo apt install $DEB"
