# Validation status

The repository contains three standalone DSP test programs and one GStreamer
property/integration test:

- `tests/test_core.cpp` covers limiter coefficients, flat programme output,
  lookahead, stereo linking, buffer partitioning, and a deterministic
  postpreemphasis stress case.
- `tests/test_clipper_core.cpp` checks the hard constraint against an
  independent preemphasis implementation, including 44.1, 48, and 96 kHz,
  state resets, channel independence, bypass, and diagnostics.
- `tests/test_extensions.cpp` covers true-peak and soft-knee configurations,
  partition invariance, latency, bypass alignment, transfer shape, and
  non-finite input handling across rates from 8 to 384 kHz at core level.
- `tests/test_properties.cpp` checks discovery, new property metadata and
  readback, pipeline execution, latency reporting, and inactive-knee state.

`make test` runs the three GStreamer-independent programs.
`make integration-test` builds both plugins, runs the GObject integration
test, and executes both smoke-test scripts using an isolated local registry.

## Recorded target validation

True-peak and soft-knee work was previously built and measured on a Raspberry
Pi 3 Model B Plus running 64-bit Linux with GCC 14.2.0 and GStreamer 1.26.2.
That work included core tests, property/integration tests, plugin discovery,
smoke pipelines, latency checks, and offline signal analysis.

Those measurements predate the later removal of `safety-margin-db`, the direct
`threshold-dbfs` semantics, and the addition of `hf-correction-us`. Historical
benchmark and analysis files in this repository must not be interpreted as
measurements of the current HEAD.

## Not yet verified for current HEAD

- A clean build and full runtime check on a freshly installed Raspberry Pi OS
  Lite 64-bit system after the latest API cleanup.
- The DSP effect of `hf-correction-us` on the target; current automated
  coverage checks its GObject type, range, default, and readback only.
- Long-duration operation inside the full FMheadend application.
- Absolute RF deviation, the complete transmitter signal path, or subjective
  listening quality.
- General support across Linux distributions, non-ARM targets, or Windows.

For a current release candidate, run the build, test, install, `gst-inspect`,
and smoke-test commands in the root README on the target system and record the
tested commit and platform.
