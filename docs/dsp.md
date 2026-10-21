# DSP reference

Both plugins process interleaved stereo F32LE audio and keep the programme
path flat. FM preemphasis exists only in a detector or clipping constraint;
the plugins do not apply a preemphasis -> nonlinear processor -> deemphasis
chain to the programme audio.

## Preemphasis weighting

For sample rate `Fs` and time constant `tau`, the shared first-order model is:

```text
r = exp(-1 / (Fs * tau))
g = 1 / (1 - r)
y[n] = g*x[n] - g*r*x[n-1]
```

At 48 kHz the coefficients are approximately:

- 50 µs: `r=0.6592406302`, `g=2.9346221663`
- 75 µs: `r=0.7574651284`, `g=4.1231184340`

`weighting-us=0` reduces the model to `r=0`, `g=1`. The supported nominal
values are exactly 0, 50, and 75 µs.

This is a software estimate of a downstream preemphasis response. It is not a
complete model of a transmitter or an RF-deviation guarantee.

## Limiter

`fmheadendlimiter` delays the programme signal by `lookahead-ms`. For each
incoming frame it converts the detector magnitude to a maximum safe gain.
`stereo-link-percent` interpolates each channel's gain requirement toward the
stricter channel; at 100% both channels receive the linked requirement.

Attack shaping examines the current requirement and future requirements over
`attack-ms`, limited to the available lookahead. Release is a one-pole
recovery for which `release-ms` means 90% recovery toward the current safe
target.

`threshold-dbfs` directly selects the detector threshold. Time-varying gain
before physical preemphasis can create peaks not predicted by simply scaling
an already weighted waveform, so a lower configured threshold may be needed
for headroom. There is no `safety-margin-db` property.

With the ordinary detector, latency is the rounded lookahead in frames. The
delay remains present in bypass. The limiter is designed for continuous
streams and does not synthesize the delayed tail after EOS, so a finite input
loses its final latency-sized tail unless the caller appends suitable input.

## True-peak sidechain estimate

`true-peak-detection=true` enables a four-phase, 4× reconstructed detector.
Each phase uses a normalized 33-coefficient Blackman-windowed sinc, and the
interpolation is fused with the preemphasis difference equation. Programme
audio itself remains at the native sample rate; it is not passed through an
upsample/process/downsample path.

The detector adds 16 native sample frames to the programme delay and reported
latency, including in bypass. At 48 kHz this is approximately 0.333 ms.

The estimate is finite-resolution, not an exact continuous-time peak bound.
Its short FIR transition band is least reliable close to native Nyquist, and
time-varying gain can create additional output peaks. The feature improves
the detector's view between native sample positions but does not turn the
limiter threshold into an unconditional reconstructed-output ceiling.

## Clipper hard mode

`fmheadendclipper` uses the previous actual emitted sample to constrain the
current flat PCM sample. For linear threshold `T`:

```text
x_min = r*x_previous_output - T/g
x_max = r*x_previous_output + T/g
x_out = clamp(x_in, x_min, x_max)
```

Feeding the actual output back into the model keeps its state aligned with
the samples sent downstream. With zero weighting this becomes ordinary flat
clipping to `[-T,+T]`.

Channels are constrained independently; there is no stereo link. The clipper
has no lookahead, attack, or release and adds no algorithmic latency. It is a
last protection stage, not a loudness processor. Placing it after the
low-pass filter allows it to catch sample peak regrowth caused by that filter.

The constraint applies at discrete weighted sample positions. It does not
protect reconstructed intersample peaks in either clipping mode.

## Soft mode

For knee width `K`, let `a=T*10^(-K/20)`, `D=T-a`, and let
`u=|g*(input-r*previous_output)|`. Soft mode maps the weighted magnitude as:

```text
u <= a:        f(u) = u
a < u < a+2D: f(u) = a + (u-a) - (u-a)^2/(4D)
u >= a+2D:    f(u) = T
```

The sign is restored and the flat PCM output is recovered from the weighted
value. The transfer is monotone and joins its identity and ceiling sections
with a continuous first derivative. `soft-knee-db=0` uses the hard path.

Floating-point conversion may exceed the ideal discrete weighted ceiling by
a few rounding units. Soft mode may reduce some high-order artifacts, but it
modifies more samples and is not guaranteed to reduce every distortion
metric.

## HF correction

The clipper calculates its effective weighting at configuration time:

```text
effective_weighting_us =
    weighting_us == 0 ? 0 : weighting_us + hf_correction_us
```

The correction range is -10 to +30 µs, producing effective nonzero values
from 40 to 105 µs. It affects both hard and soft modes, adds no filter stage
or latency, and does not change the limiter or the transmitter configuration.
The resulting ceiling is relative to this corrected software model.

## State and telemetry

Property changes are applied before the next buffer and normally reconfigure
and reset DSP state. Changing an inactive `soft-knee-db` while the clipper is
in hard mode is the exception and preserves state.

A buffer carrying `GST_BUFFER_FLAG_DISCONT` resets detector, envelope, delay,
or feedback state as applicable. Clipper cumulative diagnostics remain across
this reset but reset on reconfiguration.

- Limiter `detector-peak-dbfs` reports the most recent buffer's input
  sidechain peak.
- Limiter `gain-reduction-db` reports the most recent buffer's calculated
  reduction, including when bypass prevents it from being applied.
- Clipper `detector-peak-dbfs` reports the actual output's weighted peak.
- `clip-count` counts every changed channel sample, including soft-knee-only
  changes.
- `max-clipping-db` is cumulative since reconfiguration.
- `buffer-max-clipping-db` describes only the most recently processed buffer.

## System-level limitations

Software detector thresholds, reconstructed digital peaks, and measured FM
deviation are different quantities. Downstream gain, filters, DAC behaviour,
the modulator, and transmitter hardware can all change the final result.
Settings therefore require validation on the complete FMheadend signal path.
