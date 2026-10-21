#pragma once

#include <cmath>

// Shared FM pre-emphasis model used by both the lookahead limiter and the
// hard clipper. This models the QN8007's internal (non-defeatable) 50/75 us
// first-order pre-emphasis network so that both DSP cores can estimate what
// a flat PCM sample stream will look like after the transmitter applies it.
//
// For time constant tau and sample rate Fs:
//   r = exp(-1 / (Fs * tau))
//   g = 1 / (1 - r)
//   y[n] = g*x[n] - g*r*x[n-1]
//
// These coefficients and the difference equation are validated (see
// docs/DSP_DESIGN.md); do not change the model here.

namespace fmheadend {

struct DetectorCoefficients {
    double r = 0.0;
    double g = 1.0;
};

inline DetectorCoefficients preemphasis_coefficients(double sample_rate, double tau_seconds) {
    if (tau_seconds <= 0.0)
        return {};
    const double r = std::exp(-1.0 / (sample_rate * tau_seconds));
    const double g = 1.0 / (1.0 - r);
    return {r, g};
}

// Stateful one-pole pre-emphasis estimator. Feeding it the samples that will
// actually reach the transmitter (i.e. the already-limited or already-clipped
// programme audio) yields an estimate of the post-pre-emphasis signal without
// altering the flat programme path itself.
class PreemphasisDetector {
public:
    void configure(double sample_rate, int weighting_us) {
        weighting_us_ = weighting_us;
        if (weighting_us == 0) {
            coeff_ = {};
        } else {
            coeff_ = preemphasis_coefficients(sample_rate, weighting_us * 1e-6);
        }
        reset();
    }

    void reset() {
        prev_[0] = 0.0f;
        prev_[1] = 0.0f;
    }

    inline float process(unsigned ch, float x) {
        if (weighting_us_ == 0)
            return x;
        const float y = static_cast<float>(coeff_.g * x - coeff_.g * coeff_.r * prev_[ch]);
        prev_[ch] = x;
        return y;
    }

    // Value most recently fed to process() for this channel (i.e. the
    // internal pre-emphasis filter state x[n-1]). Used by callers that need
    // to derive a bound on the *next* sample before calling process(), such
    // as the FM-aware hard clipper.
    float previous_output(unsigned ch) const { return prev_[ch]; }

    DetectorCoefficients coefficients() const { return coeff_; }

private:
    int weighting_us_ = 0;
    DetectorCoefficients coeff_{};
    float prev_[2] = {0.0f, 0.0f};
};

}  // namespace fmheadend
