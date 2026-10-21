#!/usr/bin/env bash
set -euo pipefail

ELEMENT="fmheadendclipper"
CAPS='audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved'

if ! gst-inspect-1.0 "$ELEMENT" >/dev/null 2>&1; then
  echo "ERROR: GStreamer element '$ELEMENT' is not installed or not discoverable." >&2
  exit 1
fi

echo "[1/4] Weighted (50 us) hard-clip smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=10000 volume=0.90 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-9 weighting-us=50 \
  ! fakesink sync=false

echo "[2/4] Weighted (75 us) hard-clip smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=10000 volume=0.90 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-9 weighting-us=75 \
  ! fakesink sync=false

echo "[3/4] OFF-weighting (flat hard-clip) and bypass smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=1000 volume=0.90 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-9 weighting-us=0 bypass=false \
  ! "$ELEMENT" threshold-dbfs=-9 weighting-us=50 bypass=true \
  ! fakesink sync=false

echo "[4/4] Soft-knee smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=30 samplesperbuffer=480 freq=10000 volume=0.90 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-9 weighting-us=75 clipping-mode=soft soft-knee-db=2 \
  ! fakesink sync=false

echo "PASS: GStreamer clipper smoke tests completed without negotiation/runtime errors."
