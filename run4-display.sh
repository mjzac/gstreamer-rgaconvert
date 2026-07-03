#!/usr/bin/env bash
#
# Display N cameras in a grid on the DRM/KMS console (no X needed), each scaled
# by rgaconvert (RGA hardware) then composited and scanned out via kmssink.
#
# The per-camera NV12->BGRx convert is zero-copy dma-buf; the final grid
# composite is done by `compositor` (CPU blend) because only one process can be
# DRM master, so we cannot give each camera its own KMS plane from gst-launch.
#
# Usage:
#   ./run4-display.sh                 # 2x2 grid of the 4 cameras on HDMI
#   COLS=2 CELL_W=640 CELL_H=360 ./run4-display.sh
#   DEVICES="/dev/video11 /dev/video12" COLS=2 ./run4-display.sh
#   NUM_BUFFERS=60 ./run4-display.sh  # bounded run (for a quick test)
#   SINK="fbdevsink" ./run4-display.sh   # alternative sink
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-$SCRIPT_DIR/build/plugins}"

read -r -a DEVICES <<< "${DEVICES:-/dev/video11 /dev/video12 /dev/video13 /dev/video14}"
CELL_W=${CELL_W:-640}      # per-camera cell width
CELL_H=${CELL_H:-360}      # per-camera cell height
COLS=${COLS:-2}            # grid columns
CAM_W=${CAM_W:-1920}       # camera capture width
CAM_H=${CAM_H:-1080}       # camera capture height
SINK=${SINK:-kmssink}      # display sink (kmssink uses the existing mode + scales)
NUM_BUFFERS=${NUM_BUFFERS:-0}   # 0 = run forever (Ctrl-C to stop)

n=${#DEVICES[@]}
rows=$(( (n + COLS - 1) / COLS ))
OUT_W=$(( CELL_W * COLS ))
OUT_H=$(( CELL_H * rows ))

# compositor element with one positioned sink pad per camera.
comp=(compositor name=mix)
for i in "${!DEVICES[@]}"; do
  x=$(( (i % COLS) * CELL_W ))
  y=$(( (i / COLS) * CELL_H ))
  comp+=("sink_${i}::xpos=$x" "sink_${i}::ypos=$y")
done

args=("${comp[@]}" ! "video/x-raw,format=BGRx,width=$OUT_W,height=$OUT_H" ! $SINK)

for i in "${!DEVICES[@]}"; do
  args+=(v4l2src device="${DEVICES[$i]}" io-mode=dmabuf)
  [ "$NUM_BUFFERS" -gt 0 ] && args+=(num-buffers="$NUM_BUFFERS")
  args+=(! "video/x-raw,format=NV12,width=$CAM_W,height=$CAM_H"
         ! rgaconvert
         ! "video/x-raw,format=BGRx,width=$CELL_W,height=$CELL_H"
         ! "mix.sink_${i}")
done

echo "GST_PLUGIN_PATH=$GST_PLUGIN_PATH"
echo "displaying $n cameras: ${DEVICES[*]}"
echo "grid ${COLS}x${rows} cell ${CELL_W}x${CELL_H} -> ${OUT_W}x${OUT_H} on $SINK (num-buffers=$NUM_BUFFERS)"
exec gst-launch-1.0 -e "${args[@]}"
