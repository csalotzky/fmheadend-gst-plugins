#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "fm_preemphasis.hpp"

// FM-aware hard clipper core.
//
// Unlike fmheadendlimiter, this is not a broadband gain-reduction stage. It
// is intended as the last line of defence against rare, very short residual
// peaks that survive the limiter and a downstream low-pass filter (which can
// itself cause peak regrowth). See docs/CLIPPER_DESIGN.md.
//
// The clipper keeps the programme path flat PCM, exactly like the limiter,
// but constrains the *estimated* post-pre-emphasis signal using the same
// validated one-pole model as the limiter (see fm_preemphasis.hpp):
//
//   y[n] = g*x[n] - g*r*x[n-1]
//
// For a target linear threshold T, requiring |y[n]| <= T gives a bound on
// the current flat sample given the *actual, already-clipped* previous
// output x[n-1]:
//
//   x_min = r*x[n-1] - T/g
//   x_max = r*x[n-1] + T/g
//   x_out = clamp(x_in, x_min, x_max)
//
// Using the actual previous output (rather than the unclipped input) for
// x[n-1] is essential: it keeps the internal virtual pre-emphasis state in
// sync with what the transmitter will actually see.
//
// With weighting-us=0 the shared PreemphasisDetector reports r=0, g=1, which
// reduces the bound to a plain x_min=-T, x_max=+T flat hard clip -- the same
// code path handles both cases.
//
// Stereo channels are clipped independently (no stereo link). An exact,
// provably-correct 100% linked FM-aware constraint was not attempted; see
// docs/CLIPPER_DESIGN.md for the rationale.

namespace fmheadend {

enum class ClippingMode { Hard = 0, Soft = 1 };

struct ClipperParams {
    float threshold_dbfs = -9.0f;
    int weighting_us = 50;  // 0, 50 or 75
    bool bypass = false;
    ClippingMode clipping_mode = ClippingMode::Hard;
    float soft_knee_db = 2.0f;
    int hf_correction_us = 0;  // -10..30; ignored when weighting_us is zero
};

class FmClipperCore {
public:
    bool configure(unsigned sample_rate, const ClipperParams &params) {
        if (sample_rate < 8000 || sample_rate > 384000)
            return false;
        if (params.weighting_us != 0 && params.weighting_us != 50 && params.weighting_us != 75)
            return false;
        if (params.hf_correction_us < -10 || params.hf_correction_us > 30)
            return false;
        if (!(params.threshold_dbfs <= 0.0f && params.threshold_dbfs >= -60.0f))
            return false;
        if (!(params.soft_knee_db >= 0.0f && params.soft_knee_db <= 6.0f) ||
            (params.clipping_mode != ClippingMode::Hard && params.clipping_mode != ClippingMode::Soft))
            return false;

        sample_rate_ = sample_rate;
        params_ = {params.threshold_dbfs, params.weighting_us, params.bypass};
        soft_active_ = params.clipping_mode == ClippingMode::Soft &&
                       params.soft_knee_db > 0 && !params.bypass;
        threshold_linear_ = db_to_gain(params.threshold_dbfs);
        knee_start_ = threshold_linear_ * db_to_gain(-params.soft_knee_db);
        const double width = threshold_linear_ - knee_start_;
        knee_end_ = knee_start_ + 2.0 * width;
        knee_factor_ = width > 0 ? 1.0 / (4.0 * width) : 0;
        inverse_threshold_ = 1.0 / threshold_linear_;

        const int effective_weighting_us = params.weighting_us == 0
            ? 0 : params.weighting_us + params.hf_correction_us;
        detector_.configure(sample_rate_, effective_weighting_us);

        reset_state();
        return true;
    }

    // Resets internal DSP filter state (pre-emphasis memory). Cumulative
    // diagnostics (clip_count(), max_clipping_db()) are intentionally left
    // untouched; see docs/CLIPPER_DESIGN.md for why they are running totals
    // rather than per-buffer values.
    void reset() {
        detector_.reset();
        last_detector_peak_ = 0.0f;
        buffer_max_clipping_db_ = 0.0f;
    }

    void process(float *interleaved_stereo, std::size_t frames) {
        if (!interleaved_stereo || sample_rate_ == 0)
            return;
        if (soft_active_) {
            process_soft(interleaved_stereo, frames);
            return;
        }

        const auto coeff = detector_.coefficients();
        const float inv_g = static_cast<float>(1.0 / coeff.g);
        const float r = static_cast<float>(coeff.r);
        // Keep telemetry-only intermediates from changing bound evaluation.
        const volatile float telemetry_half_width = threshold_linear_ * inv_g;
        const float inv_half_width = 1.0f / telemetry_half_width;

        float peak_this_call = 0.0f;
        float max_overshoot_ratio_this_call = 1.0f;
        bool clipped_this_call = false;

        for (std::size_t i = 0; i < frames; ++i) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                const float in_v = sanitize(interleaved_stereo[2 * i + ch]);

                const float prev = detector_.previous_output(ch);
                const float x_min = r * prev - threshold_linear_ * inv_g;
                const float x_max = r * prev + threshold_linear_ * inv_g;

                float out_v = in_v;
                if (!params_.bypass)
                    out_v = std::clamp(in_v, x_min, x_max);

                const float y = detector_.process(ch, out_v);
                peak_this_call = std::max(peak_this_call, std::fabs(y));

                if (!params_.bypass && out_v != in_v) {
                    ++clip_count_;

                    const volatile float telemetry_r = r;
                    const volatile float telemetry_prev = prev;
                    const float overshoot_ratio =
                        std::fabs(in_v - telemetry_r * telemetry_prev) * inv_half_width;
                    max_overshoot_ratio_this_call =
                        std::max(max_overshoot_ratio_this_call, overshoot_ratio);
                    clipped_this_call = true;
                }

                interleaved_stereo[2 * i + ch] = out_v;
            }
        }

        last_detector_peak_ = peak_this_call;
        buffer_max_clipping_db_ = clipped_this_call
            ? std::max(0.0f, 20.0f * std::log10(max_overshoot_ratio_this_call))
            : 0.0f;
        max_clipping_db_ = std::max(max_clipping_db_, buffer_max_clipping_db_);
    }

    unsigned sample_rate() const { return sample_rate_; }
    float threshold_linear() const { return threshold_linear_; }
    float last_detector_peak_dbfs() const { return gain_to_db(last_detector_peak_); }
    std::uint64_t clip_count() const { return clip_count_; }
    float max_clipping_db() const { return max_clipping_db_; }
    float buffer_max_clipping_db() const { return buffer_max_clipping_db_; }

private:
    // One call per soft-mode buffer keeps the new knee's working registers
    // separate from the unchanged inline hard path in GStreamer callers.
#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    void process_soft(float *audio, std::size_t frames) {
        const auto c = detector_.coefficients();
        const double inv_g = 1.0 / c.g;
        double peak = 0.0;
        double ratio = 1.0;
        for (std::size_t i = 0; i < frames; ++i) {
          for (unsigned ch = 0; ch < 2; ++ch) {
            const float input = sanitize(audio[2 * i + ch]);
            const double centre = c.r * detector_.previous_output(ch);
            const double v = c.g * (input - centre);
            const double u = std::fabs(v);
            float output = input; // exact identity below knee, including rounding
            if (u > knee_start_) {
                const double z = u - knee_start_;
                const double mapped = u >= knee_end_ ? threshold_linear_ :
                    std::min<double>(threshold_linear_, knee_start_ + z - z * z * knee_factor_);
                output = static_cast<float>(centre + std::copysign(mapped, v) * inv_g);
                if (output != input) ++clip_count_;
                ratio = std::max(ratio, u * inverse_threshold_);
            }
            peak = std::max(peak, static_cast<double>(std::fabs(detector_.process(ch, output))));
            audio[2 * i + ch] = output;
          }
        }
        last_detector_peak_ = static_cast<float>(peak);
        buffer_max_clipping_db_ = static_cast<float>(20.0 * std::log10(ratio));
        max_clipping_db_ = std::max(max_clipping_db_, buffer_max_clipping_db_);
    }

    static float db_to_gain(float db) {
        return std::pow(10.0f, db / 20.0f);
    }

    static float gain_to_db(float gain) {
        if (!(gain > 1e-10f))
            return -200.0f;
        return std::max(-200.0f, 20.0f * std::log10(gain));
    }

    static float sanitize(float x) {
        return std::isfinite(x) ? x : 0.0f;
    }

    void reset_state() {
        detector_.reset();
        last_detector_peak_ = 0.0f;
        clip_count_ = 0;
        max_clipping_db_ = 0.0f;
        buffer_max_clipping_db_ = 0.0f;
    }

    unsigned sample_rate_ = 0;
    // Preserve original hot-state offsets; optional knee state belongs after
    // the legacy detector and diagnostics, not between them and the controls.
    struct LegacyHotParams {
        float threshold_dbfs = -9.0f;
        int weighting_us = 50;
        bool bypass = false;
    } params_{};
    float threshold_linear_ = 1.0f;

    PreemphasisDetector detector_{};

    float last_detector_peak_ = 0.0f;
    std::uint64_t clip_count_ = 0;
    float max_clipping_db_ = 0.0f;
    float buffer_max_clipping_db_ = 0.0f;

    bool soft_active_ = false;
    double knee_start_ = 0, knee_end_ = 0, knee_factor_ = 0, inverse_threshold_ = 1;
};

}  // namespace fmheadend
