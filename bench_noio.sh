#!/bin/bash
WORKDIR="/var/folders/z8/l57rl71s08b0wmtwhjgjv9_80000gn/T/tmp.ySKQJz2iaZ"
LDECOD="/Users/matthewmoukaddem/Projects/h264-tools/build/ldecod/ldecod"

# Create named pipes for view outputs so they drain instantly
rm -f "$WORKDIR/out_ViewId0000.yuv" "$WORKDIR/out_ViewId0001.yuv"
mkfifo "$WORKDIR/out_ViewId0000.yuv"
mkfifo "$WORKDIR/out_ViewId0001.yuv"
cat "$WORKDIR/out_ViewId0000.yuv" > /dev/null &
cat "$WORKDIR/out_ViewId0001.yuv" > /dev/null &

cd "$WORKDIR"
echo "=== Decode with output piped to /dev/null ==="
time "$LDECOD" -p DecodeAllLayers=1 -p InputFile="$WORKDIR/demuxed.h264" -p OutputFile="$WORKDIR/out.yuv" -p Silent=0 2>&1 | tail -5

rm -f "$WORKDIR/out_ViewId0000.yuv" "$WORKDIR/out_ViewId0001.yuv" "$WORKDIR/out.yuv"
echo "=== Done ==="
