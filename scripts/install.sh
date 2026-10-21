#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

make -j"$(nproc)" plugin
make test
sudo make install

rm -f "$HOME"/.cache/gstreamer-1.0/registry.*.bin 2>/dev/null || true

echo
gst-inspect-1.0 fmheadendlimiter

echo
gst-inspect-1.0 fmheadendclipper

echo
bash scripts/smoke_test.sh

echo
bash scripts/smoke_test_clipper.sh
