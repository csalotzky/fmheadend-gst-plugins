#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "fm_preemphasis.hpp"
#include "fm_true_peak.hpp"

namespace fmheadend {

struct LimiterParams {
    float threshold_dbfs = -9.0f;
    float lookahead_ms = 5.0f;
    float attack_ms = 0.25f;
    float release_ms = 20.0f;
    float stereo_link_percent = 100.0f;
    int weighting_us = 50;  // 0, 50 or 75
    bool bypass = false;
    bool true_peak_detection = false;
};

class FmLimiterCore {
public:
    bool configure(unsigned sample_rate, const LimiterParams &params) {
        if (sample_rate < 8000 || sample_rate > 384000)
            return false;
        if (params.weighting_us != 0 && params.weighting_us != 50 && params.weighting_us != 75)
            return false;
        if (!(params.threshold_dbfs <= 0.0f && params.threshold_dbfs >= -60.0f))
            return false;
        if (params.lookahead_ms < 0.0f || params.lookahead_ms > 100.0f)
            return false;
        if (params.attack_ms < 0.0f || params.attack_ms > 100.0f)
            return false;
        if (params.release_ms <= 0.0f || params.release_ms > 5000.0f)
            return false;
        if (params.stereo_link_percent < 0.0f || params.stereo_link_percent > 100.0f)
            return false;

        sample_rate_ = sample_rate;
        params_ = params;
        threshold_linear_ = db_to_gain(params.threshold_dbfs);
        link_ = params.stereo_link_percent * 0.01f;

        lookahead_samples_ = ms_to_samples(params.lookahead_ms);
        attack_samples_ = std::min(ms_to_samples(params.attack_ms), lookahead_samples_);
        detector_delay_ = params.true_peak_detection ? TruePeakDetector::delay : 0;
        lookahead_samples_ += detector_delay_;

        // Release definition: recover 90% of the remaining distance toward
        // the current safe target during release_ms.
        const double release_samples = std::max(1.0, params.release_ms * 1e-3 * sample_rate_);
        release_alpha_ = static_cast<float>(std::exp(std::log(0.1) / release_samples));

        detector_.configure(sample_rate_, params.weighting_us);
        if (params.true_peak_detection)
            true_peak_.configure(sample_rate_, params.weighting_us);

        // L+1 lets us retain q[s..s+L] while input n=s+L is arriving.
        ring_frames_ = static_cast<std::size_t>(lookahead_samples_) + 1u;
        audio_ring_.assign(ring_frames_ * 2u, 0.0f);
        req_ring_.assign((ring_frames_ + attack_samples_) * 2u, 1.0f);
        attack_weights_.resize(static_cast<std::size_t>(attack_samples_) + 1u);
        const float inv_attack_samples =
            attack_samples_ ? (1.0f / static_cast<float>(attack_samples_)) : 0.0f;
        for (unsigned k = 0; k <= attack_samples_; ++k)
            attack_weights_[k] = 1.0f - static_cast<float>(k) * inv_attack_samples;

        reset_state();
        return true;
    }

    void reset() {
        std::fill(audio_ring_.begin(), audio_ring_.end(), 0.0f);
        std::fill(req_ring_.begin(), req_ring_.end(), 1.0f);
        detector_.reset();
        true_peak_.reset();
        reset_state();
    }

    void process(float *interleaved_stereo, std::size_t frames) {
        if (params_.true_peak_detection)
            process_true_peak(interleaved_stereo, frames);
        else
            process_impl<false>(interleaved_stereo, frames);
    }

private:
    // Keep the FIR's register/stack requirements out of callers of the legacy
    // inline loop. This is one call per buffer, never one call per sample.
#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    void process_true_peak(float *audio, std::size_t frames) {
        process_impl<true>(audio, frames);
    }

    template<bool TruePeak>
    void process_impl(float *interleaved_stereo, std::size_t frames) {
        if (!interleaved_stereo || sample_rate_ == 0 || ring_frames_ == 0)
            return;

        float block_detector_peak = 0.0f;
        float block_min_gain = 1.0f;

        for (std::size_t i = 0; i < frames; ++i, ++sample_index_) {
            const float in_l = sanitize(interleaved_stereo[2 * i]);
            const float in_r = sanitize(interleaved_stereo[2 * i + 1]);

            float det_l, det_r;
            if constexpr (TruePeak) {
                true_peak_.process(in_l, in_r, det_l, det_r);
            } else {
                det_l = std::fabs(detector_.process(0, in_l));
                det_r = std::fabs(detector_.process(1, in_r));
            }

            block_detector_peak =
                std::max(block_detector_peak, std::max(det_l, det_r));

            float req_l = required_gain(det_l);
            float req_r = required_gain(det_r);

            const float linked = std::min(req_l, req_r);
            req_l += (linked - req_l) * link_;
            req_r += (linked - req_r) * link_;

            const std::size_t in_slot = input_slot_;

            audio_ring_[2 * in_slot] = in_l;
            audio_ring_[2 * in_slot + 1] = in_r;

            std::size_t req_slot = in_slot;
            if constexpr (TruePeak) {
                req_slot = (in_slot >= detector_delay_)
                    ? in_slot - detector_delay_
                    : in_slot + ring_frames_ - detector_delay_;
            }
            req_ring_[2 * req_slot] = req_l;
            req_ring_[2 * req_slot + 1] = req_r;
            if (req_slot < attack_samples_) {
                const std::size_t mirrored_slot = ring_frames_ + req_slot;
                req_ring_[2 * mirrored_slot] = req_l;
                req_ring_[2 * mirrored_slot + 1] = req_r;
            }
            input_slot_ = next_slot(in_slot);

            if (sample_index_ < lookahead_samples_) {
                interleaved_stereo[2 * i] = 0.0f;
                interleaved_stereo[2 * i + 1] = 0.0f;
                continue;
            }

            // ring_frames_ is lookahead_samples_ + 1, so the delayed
            // sample is always the slot immediately after the input slot.
            const std::size_t out_slot = input_slot_;
            float target_l = 1.0f;
            float target_r = 1.0f;
            attack_targets(out_slot, target_l, target_r);

            const float gain_l = update_release(0, target_l);
            const float gain_r = update_release(1, target_r);

            float out_l = audio_ring_[2 * out_slot];
            float out_r = audio_ring_[2 * out_slot + 1];

            if (!params_.bypass) {
                out_l *= gain_l;
                out_r *= gain_r;
            }

            interleaved_stereo[2 * i] = out_l;
            interleaved_stereo[2 * i + 1] = out_r;

            block_min_gain = std::min(block_min_gain, std::min(gain_l, gain_r));
        }

        if (frames > 0) {
            last_detector_peak_ = block_detector_peak;
            last_gain_reduction_db_ =
                -20.0f * std::log10(std::max(1e-12f, block_min_gain));
        }
    }

public:
    unsigned sample_rate() const { return sample_rate_; }
    unsigned lookahead_samples() const { return lookahead_samples_; }
    float latency_ms() const {
        return sample_rate_ ? (1000.0f * static_cast<float>(lookahead_samples_) /
                               static_cast<float>(sample_rate_)) : 0.0f;
    }
    float last_detector_peak_dbfs() const {
        return gain_to_db(last_detector_peak_);
    }
    float last_gain_reduction_db() const { return last_gain_reduction_db_; }
    float threshold_linear() const { return threshold_linear_; }

private:
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

    unsigned ms_to_samples(float ms) const {
        if (ms <= 0.0f)
            return 0;
        return static_cast<unsigned>(std::llround(ms * 1e-3 * sample_rate_));
    }

    float required_gain(float detector_peak) const {
        if (!(detector_peak > threshold_linear_))
            return 1.0f;
        return std::clamp(threshold_linear_ / detector_peak, 0.0f, 1.0f);
    }

    // Safe attack shaping with lookahead:
    // q[s] is the per-sample maximum allowed gain. For output sample s,
    // inspect q[s..s+A]. Each future constraint contributes a linearly
    // increasing attenuation as it approaches. k=0 always contributes q[s],
    // therefore target <= q[s] and the detector threshold cannot be exceeded
    // merely because of attack smoothing.
    std::size_t next_slot(std::size_t slot) const {
        ++slot;
        return (slot == ring_frames_) ? 0u : slot;
    }

    void attack_targets(std::size_t out_slot, float &target_l, float &target_r) const {
        if (attack_samples_ == 0) {
            target_l = req_ring_[2 * out_slot];
            target_r = req_ring_[2 * out_slot + 1];
            return;
        }

        const float *req = &req_ring_[2 * out_slot];
        unsigned k = 0;

#if defined(__ARM_NEON)
        const unsigned attack_weight_count = attack_samples_ + 1u;
        const float32x4_t one = vdupq_n_f32(1.0f);
        float32x4_t min_l = one;
        float32x4_t min_r = one;

        for (; k + 4u <= attack_weight_count; k += 4u) {
            const float32x4x2_t q = vld2q_f32(req);
            const float32x4_t weight = vld1q_f32(&attack_weights_[k]);
            const float32x4_t bound_l =
                vfmsq_f32(one, vsubq_f32(one, q.val[0]), weight);
            const float32x4_t bound_r =
                vfmsq_f32(one, vsubq_f32(one, q.val[1]), weight);
            min_l = vminq_f32(min_l, bound_l);
            min_r = vminq_f32(min_r, bound_r);
            req += 8;
        }

        target_l = std::min(target_l, vminvq_f32(min_l));
        target_r = std::min(target_r, vminvq_f32(min_r));
#endif

        for (; k <= attack_samples_; ++k) {
            const float weight = attack_weights_[k];
            const float q_l = req[0];
            const float q_r = req[1];
            const float bound_l = 1.0f - (1.0f - q_l) * weight;
            const float bound_r = 1.0f - (1.0f - q_r) * weight;
            target_l = std::min(target_l, bound_l);
            target_r = std::min(target_r, bound_r);
            req += 2;
        }
        target_l = std::clamp(target_l, 0.0f, 1.0f);
        target_r = std::clamp(target_r, 0.0f, 1.0f);
    }

    float update_release(unsigned ch, float target) {
        float &env = envelope_[ch];
        if (target < env) {
            env = target;
        } else if (env < target) {
            env = target - (target - env) * release_alpha_;
        }
        env = std::clamp(env, 0.0f, target);
        return env;
    }

    void reset_state() {
        sample_index_ = 0;
        input_slot_ = 0;
        envelope_[0] = 1.0f;
        envelope_[1] = 1.0f;
        last_detector_peak_ = 0.0f;
        last_gain_reduction_db_ = 0.0f;
    }

    unsigned sample_rate_ = 0;
    LimiterParams params_{};
    float threshold_linear_ = 1.0f;
    float link_ = 1.0f;
    unsigned lookahead_samples_ = 0;
    unsigned attack_samples_ = 0;
    float release_alpha_ = 0.0f;

    PreemphasisDetector detector_{};
    std::vector<float> audio_ring_{};
    std::vector<float> req_ring_{};
    std::vector<float> attack_weights_{};
    std::size_t ring_frames_ = 0;
    std::uint64_t sample_index_ = 0;
    std::size_t input_slot_ = 0;
    float envelope_[2] = {1.0f, 1.0f};

    float last_detector_peak_ = 0.0f;
    float last_gain_reduction_db_ = 0.0f;

    // Keep legacy hot-state offsets/cache locality intact. The optional FIR
    // must not split the detector, ring pointers and envelope across memory.
    TruePeakDetector true_peak_{};
    unsigned detector_delay_ = 0;
};
}  // namespace fmheadend
