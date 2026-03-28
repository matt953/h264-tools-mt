#!/bin/bash
set -e

WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

LDECOD="/Users/matthewmoukaddem/Projects/h264-tools/build/ldecod/ldecod"
INPUT='/Volumes/Movies/Tron Legacy (2010)/.original/Tron Legacy (2010) {edition-3d} copy.mkv'

echo "=== Step 1: Demux 30s H.264 to temp file ==="
ffmpeg -y -t 30 -i "$INPUT" -f h264 -c:v copy -bsf:v h264_mp4toannexb "$WORKDIR/demuxed.h264" 2>/dev/null
echo "Demuxed: $(du -h "$WORKDIR/demuxed.h264" | cut -f1)"

echo "=== Step 2: Decode with OpenMP ldecod ==="
echo "OMP_NUM_THREADS=${OMP_NUM_THREADS:-default}"

cd "$WORKDIR"
time "$LDECOD" -p DecodeAllLayers=1 -p InputFile="$WORKDIR/demuxed.h264" -p OutputFile="$WORKDIR/dec.yuv" -p Silent=0 2>&1 | tail -8

echo "=== Cleanup ==="
