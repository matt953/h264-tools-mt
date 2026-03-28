#!/bin/bash
set -e

LDECOD="/Users/matthewmoukaddem/Projects/h264-tools/build/ldecod/ldecod"
YUVSBSPIPE="/Users/matthewmoukaddem/Projects/h264-tools/build/tools/yuvsbspipe"

INPUT='/Volumes/Movies/Tron Legacy (2010)/.original/Tron Legacy (2010) {edition-3d} copy.mkv'
OUTPUT='/Volumes/Movies/Tron Legacy (2010)/.original/Tron Legacy (2010) {edition-3d} h264tools_test.mp4'

FSWIDTH=1920
FSHEIGHT=1080
SBSWIDTH=3840
FRAMERATE="24000/1001"

# Duration: 2 minutes
DURATION=120

# Clean up any leftover fifos
rm -f /tmp/dec_ViewId0000.yuv /tmp/dec_ViewId0001.yuv /tmp/sbs.yuv /tmp/demuxed.h264

# Create named pipes
mkfifo /tmp/dec_ViewId0000.yuv
mkfifo /tmp/dec_ViewId0001.yuv
mkfifo /tmp/sbs.yuv
mkfifo /tmp/demuxed.h264

echo "=== Step 1: Demuxing H.264 stream ==="
ffmpeg -y -t $DURATION -i "$INPUT" -f h264 -c:v copy -bsf:v h264_mp4toannexb /tmp/demuxed.h264 &
PID_DEMUX=$!
sleep 2

echo "=== Step 2: Decoding MVC with ldecod ==="
$LDECOD -p DecodeAllLayers=1 -p InputFile=/tmp/demuxed.h264 -p OutputFile=/tmp/dec.yuv -p Silent=1 &
PID_LDECOD=$!
sleep 2

echo "=== Step 3: Combining views with yuvsbspipe ==="
$YUVSBSPIPE -w $FSWIDTH -h $FSHEIGHT -l /tmp/dec_ViewId0000.yuv -r /tmp/dec_ViewId0001.yuv -sbs -o /tmp/sbs.yuv &
PID_SBS=$!
sleep 2

echo "=== Step 4: Encoding SBS with VideoToolbox HEVC at 60Mbps ==="
ffmpeg -y -f rawvideo -pixel_format yuv420p -video_size ${SBSWIDTH}x${FSHEIGHT} -framerate $FRAMERATE -i /tmp/sbs.yuv \
  -i "$INPUT" \
  -t $DURATION \
  -c:v hevc_videotoolbox -b:v 60M \
  -map 0:v:0 \
  -c:a aac -map 1:a \
  -map 1:s? -c:s copy \
  "$OUTPUT"

echo "=== Cleaning up ==="
rm -f /tmp/dec_ViewId0000.yuv /tmp/dec_ViewId0001.yuv /tmp/sbs.yuv /tmp/demuxed.h264

echo "=== Done! Output: $OUTPUT ==="
