# FMheadend GStreamer audio plugins

This repository contains two native GStreamer audio filters for the
FMheadend signal chain:

- `fmheadendlimiter` is a stereo lookahead limiter. Its detector can model
  50 or 75 µs FM preemphasis while the programme output remains flat PCM.
- `fmheadendclipper` is a zero-latency final protection stage for short peaks
  that remain after limiting and low-pass filtering. It supports a hard
  constraint and an optional weighted soft knee.

The filters are intended primarily for FMheadend on Raspberry Pi OS and
Debian-based Linux. They estimate a software preemphasis-weighted signal;
neither plugin by itself guarantees reconstructed analogue peaks or RF
deviation.

The intended order is:

```text
compressor -> gain -> fmheadendlimiter -> low-pass filter -> fmheadendclipper -> output trim -> modulator
```

Both plugin descriptors currently report project version `0.1.0`.

## Audio format

Both elements accept and produce:

```text
audio/x-raw, format=F32LE, layout=interleaved, channels=2, rate=8000..192000
```

The normal FMheadend deployment format is 48 kHz stereo.

## Requirements

The build uses GNU Make, a C++17 compiler, `pkg-config`, and the GStreamer
core, base, and audio development files. Runtime verification also uses
`gst-inspect-1.0`, `gst-launch-1.0`, and elements from the GStreamer base
plugins.

On a fresh Raspberry Pi OS Lite 64-bit or Debian-based installation:

```bash
sudo apt update
sudo apt install -y \
  build-essential pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base
```

## Build and test

From the repository root:

```bash
make -j"$(nproc)" plugin
make test
```

`make plugin` builds both shared libraries in `build/`. `make test` builds and
runs the limiter, clipper, and extension DSP test executables. These tests do
not require the plugins to be installed.

To test plugin discovery and GStreamer pipelines directly from `build/`:

```bash
make integration-test
```

## Install and verify

Install both plugins into the GStreamer system plugin directory reported by
`pkg-config`:

```bash
sudo make install
rm -f "$HOME"/.cache/gstreamer-1.0/registry.*.bin
gst-inspect-1.0 fmheadendlimiter
gst-inspect-1.0 fmheadendclipper
bash scripts/smoke_test.sh
bash scripts/smoke_test_clipper.sh
```

The registry files are caches and will be recreated by GStreamer. Removing
them is normally needed only after installing or replacing a locally built
plugin.

To uninstall:

```bash
sudo make uninstall
rm -f "$HOME"/.cache/gstreamer-1.0/registry.*.bin
```

`DESTDIR` and `GST_PLUGIN_DIR` can be supplied to `make install` or
`make uninstall` for packaging or a non-default destination.

## Minimal pipeline example

This example checks negotiation and processing. A production FMheadend chain
normally has its low-pass filter between the limiter and clipper.

```bash
gst-launch-1.0 -e \
  audiotestsrc num-buffers=300 samplesperbuffer=480 freq=10000 volume=0.7 \
  ! audioconvert \
  ! 'audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved' \
  ! fmheadendlimiter threshold-dbfs=-11.5 weighting-us=50 \
  ! fmheadendclipper threshold-dbfs=-9 weighting-us=50 \
  ! fakesink sync=false
```

The thresholds above are an example with additional limiter headroom, not a
universal RF calibration. Final settings require measurement of the complete
FMheadend and transmitter chain. Do not add external preemphasis/deemphasis
filters around these elements solely to support their detectors.

## `fmheadendlimiter` properties

| Property | Type and access | Accepted values | Default | Meaning |
|---|---|---:|---:|---|
| `threshold-dbfs` | float, read/write | -60 to 0 dBFS | -9 | Weighted detector threshold. It is used directly; there is no separate safety-margin property. |
| `lookahead-ms` | float, read/write | 0 to 20 ms | 5 | Programme delay and available lookahead. |
| `attack-ms` | float, read/write | 0 to 20 ms | 0.25 | Gain-ramp attack span, internally limited to the configured lookahead. |
| `release-ms` | float, read/write | 1 to 1000 ms | 20 | Time to recover 90% of the remaining distance toward the current safe target. |
| `stereo-link-percent` | float, read/write | 0 to 100% | 100 | Blends independent channel requirements toward the stricter channel. |
| `weighting-us` | integer, read/write | exactly `0`, `50`, or `75` µs | 50 | Detector preemphasis weighting. Although GObject exposes an integer range of 0 to 75, other values are rejected and the previous value is retained. |
| `true-peak-detection` | boolean, read/write | `false` or `true` | false | Enables the 4× reconstructed sidechain estimate and adds 16 sample frames of latency. |
| `bypass` | boolean, read/write | `false` or `true` | false | Disables application of gain reduction while retaining the configured delay. |
| `gain-reduction-db` | float, read-only | 0 to 120 dB | 0 | Maximum calculated gain reduction in the most recently processed buffer. It is still calculated, but not applied, in bypass. |
| `detector-peak-dbfs` | float, read-only | -200 to 100 dBFS | -200 | Maximum input-sidechain detector level in the most recently processed buffer. |
| `latency-samples` | unsigned integer, read-only | 0 to platform maximum | 0 before setup | Current programme latency: rounded lookahead samples plus 16 frames when true-peak detection is enabled. |

`bypass=true` does not make the limiter zero-latency. At 48 kHz, the default
lookahead is 240 frames; true-peak detection increases it to 256 frames.

## `fmheadendclipper` properties

| Property | Type and access | Accepted values | Default | Meaning |
|---|---|---:|---:|---|
| `threshold-dbfs` | float, read/write | -60 to 0 dBFS | -9 | Ceiling for the discrete preemphasis-weighted sample estimate. |
| `weighting-us` | integer, read/write | exactly `0`, `50`, or `75` µs | 50 | Nominal clip-constraint weighting. Other integers in the exposed 0 to 75 range are rejected and the previous value is retained. |
| `hf-correction-us` | integer, read/write | -10 to +30 µs | 0 | Added to nonzero `weighting-us` in both clipping modes; ignored when weighting is 0. |
| `clipping-mode` | enum, read/write | `hard` (0), `soft` (1) | `hard` | Selects the direct constraint or weighted soft-knee transfer. |
| `soft-knee-db` | float, read/write | 0 to 6 dB | 2 | Knee width in soft mode. Zero selects the hard processing path. Changes while hard mode is active do not reset DSP state. |
| `bypass` | boolean, read/write | `false` or `true` | false | Disables clipping. Finite PCM samples pass unchanged; non-finite input is sanitized to zero. |
| `detector-peak-dbfs` | float, read-only | -200 to 100 dBFS | -200 | Maximum weighted level of the actual output in the most recently processed buffer. |
| `clip-count` | unsigned 64-bit, read-only | 0 to platform maximum | 0 | Running count across both channels of samples modified by hard clipping or the soft knee since reconfiguration. |
| `max-clipping-db` | float, read-only | 0 to 200 dB | 0 | Running maximum input overshoot above the weighted threshold since reconfiguration. Knee-only changes below the threshold contribute 0 dB. |
| `buffer-max-clipping-db` | float, read-only | 0 to 200 dB | 0 | Corresponding overshoot for the most recently processed buffer. |

The clipper has zero algorithmic latency and clips channels independently.
`clip-count` and `max-clipping-db` survive a GStreamer discontinuity reset but
reset after reconfiguration. `buffer-max-clipping-db` is cleared on a reset.

## Troubleshooting

- If `gst-inspect-1.0` is missing, install `gstreamer1.0-tools`.
- If a built plugin is not found before installation, use
  `GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 fmheadendlimiter` and repeat
  with `fmheadendclipper`.
- After replacing an installed plugin, remove the per-user registry cache as
  shown above and inspect both elements again.
- Build on the target architecture. The committed source is the supported
  input to the build; prebuilt test executables are not plugin packages.
- Use `GST_DEBUG=GST_PLUGIN_LOADING:6 gst-inspect-1.0 fmheadendlimiter` for
  detailed plugin-loading diagnostics.

See [DSP reference](docs/dsp.md) for processing details and limitations, and
[validation status](docs/validation.md) for the tested scope.

## License

The source is distributed under the MIT License; see [LICENSE](LICENSE).
The copyright-holder field still requires project-owner confirmation before
public release.
