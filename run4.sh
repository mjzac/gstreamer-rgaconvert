#!/usr/bin/env bash
#
# Launch N cameras through rgaconvert (NV12 -> BGRx) with zero-copy dma-buf.
#
# Usage:
#   ./run4.sh                 # run all cameras continuously (Ctrl-C to stop)
#   COUNT=1 ./run4.sh         # bounded run, print zero-copy / CPU / error counts
#
# Override defaults via environment, e.g.:
#   DEVICES="/dev/video11 /dev/video12" WIDTH=1280 HEIGHT=720 ./run4.sh
#   NUM_BUFFERS=100 ./run4.sh
#   SINK="fpsdisplaysink text-overlay=false video-sink=fakesink sync=false" ./run4.sh
#
set -euo pipefail

# Resolve the plugin build dir relative to this script (so it works from anywhere).
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$SCRIPT_DIR/build/plugins}"
export GST_DEBUG="${GST_DEBUG:-rgaconvert:6}"

# Config (all overridable from the environment).
read -r -a DEVICES <<< "${DEVICES:-/dev/video11 /dev/video12 /dev/video13 /dev/video14}"
WIDTH=${WIDTH:-1920}
HEIGHT=${HEIGHT:-1080}
SINK=${SINK:-fakesink}
COUNT=${COUNT:-0}

# In count mode we need a bounded run to get a clean total.
if [ "$COUNT" = "1" ]; then
  NUM_BUFFERS=${NUM_BUFFERS:-50}
else
  NUM_BUFFERS=${NUM_BUFFERS:-0}   # 0 = run forever
fi

# Build one independent capture->convert->sink branch per device.
args=()
for dev in "${DEVICES[@]}"; do
  args+=(v4l2src device="$dev" io-mode=dmabuf)
  [ "$NUM_BUFFERS" -gt 0 ] && args+=(num-buffers="$NUM_BUFFERS")
  args+=(! "video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT"
         ! rgaconvert
         ! "video/x-raw,format=BGRx,width=$WIDTH,height=$HEIGHT"
         ! $SINK)
done

echo "GST_PLUGIN_PATH=$GST_PLUGIN_PATH"
echo "cameras: ${DEVICES[*]}"
echo "format : NV12 ${WIDTH}x${HEIGHT} -> BGRx, num-buffers=$NUM_BUFFERS, sink=$SINK"

if [ "$COUNT" = "1" ]; then
  out=$(gst-launch-1.0 "${args[@]}" 2>&1 || true)
  expected=$(( ${#DEVICES[@]} * NUM_BUFFERS * 2 ))
  echo "----"
  echo "zero-copy imports : $(printf '%s\n' "$out" | grep -c 'zero-copy')  (expected $expected)"
  echo "CPU-fallback impts: $(printf '%s\n' "$out" | grep -c 'CPU virtual')  (expected 0)"
  echo "errors            : $(printf '%s\n' "$out" | grep -ci 'error')"
else
  exec gst-launch-1.0 -e "${args[@]}"
fi
