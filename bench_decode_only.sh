#!/bin/bash
WORKDIR="/var/folders/z8/l57rl71s08b0wmtwhjgjv9_80000gn/T/tmp.ySKQJz2iaZ"
LDECOD="/Users/matthewmoukaddem/Projects/h264-tools/build/ldecod/ldecod"

export LDECOD_NO_OUTPUT=1

# Create dummy output files (won't be written to due to LDECOD_NO_OUTPUT)
touch "$WORKDIR/out_ViewId0000.yuv" "$WORKDIR/out_ViewId0001.yuv" 2>/dev/null

cd "$WORKDIR"
echo "=== Decode only (no output at all) ==="
time "$LDECOD" -p DecodeAllLayers=1 -p InputFile="$WORKDIR/demuxed.h264" -p OutputFile="$WORKDIR/out.yuv" -p Silent=0 2>&1 | tail -5

rm -f "$WORKDIR/out_ViewId0000.yuv" "$WORKDIR/out_ViewId0001.yuv" "$WORKDIR/out.yuv"
echo "=== Done ==="
