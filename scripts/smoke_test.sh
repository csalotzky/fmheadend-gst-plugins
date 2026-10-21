#!/usr/bin/env bash
set -euo pipefail

ELEMENT="fmheadendlimiter"
CAPS='audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved'

if ! gst-inspect-1.0 "$ELEMENT" >/dev/null 2>&1; then
  echo "ERROR: GStreamer element '$ELEMENT' is not installed or not discoverable." >&2
  exit 1
fi

echo "[1/4] 1 kHz smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=1000 volume=0.20 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-11.5 lookahead-ms=5 attack-ms=0.25 release-ms=20 \
      stereo-link-percent=100 weighting-us=50 \
  ! fakesink sync=false

echo "[2/4] 10 kHz weighted-detector smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=10000 volume=0.70 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-11.5 lookahead-ms=5 attack-ms=0.25 release-ms=20 \
      stereo-link-percent=100 weighting-us=50 \
  ! fakesink sync=false

echo "[3/4] OFF-weighting smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=10000 volume=0.70 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" threshold-dbfs=-9 lookahead-ms=5 attack-ms=0.25 release-ms=20 \
      stereo-link-percent=100 weighting-us=0 \
  ! fakesink sync=false

echo "[4/4] 4x true-peak smoke test"
gst-launch-1.0 -q -e \
  audiotestsrc num-buffers=30 samplesperbuffer=480 freq=12000 volume=0.7 \
  ! audioconvert ! "$CAPS" \
  ! "$ELEMENT" true-peak-detection=true weighting-us=75 \
  ! fakesink sync=false

echo "PASS: GStreamer smoke tests completed without negotiation/runtime errors."
