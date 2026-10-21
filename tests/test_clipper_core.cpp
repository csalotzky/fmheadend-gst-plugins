#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "fm_clipper_core.hpp"

// This test file deliberately reimplements the pre-emphasis difference
// equation independently of fmheadend::PreemphasisDetector (used internally
// by FmClipperCore). Validating clipper output against a second, separately
// written reference avoids a shared-bug false pass that could occur if the
// test reused the exact same helper class that the clipper relies on.

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    } else {
        std::cout << "PASS: " << message << '\n';
    }
}

bool near(double actual, double expected, double tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

// Independent reference pre-emphasis coefficient/state computation.
struct ReferenceCoeffs {
    double r = 0.0;
    double g = 1.0;
};

ReferenceCoeffs reference_coefficients(double sample_rate, int weighting_us) {
    if (weighting_us == 0)
        return {};
    const double tau = weighting_us * 1e-6;
    const double r = std::exp(-1.0 / (sample_rate * tau));
    const double g = 1.0 / (1.0 - r);
    return {r, g};
}

// Independently re-runs the validated pre-emphasis difference equation over
// an already-produced (clipped) stereo buffer and reports the largest
// magnitude observed, to check it never exceeds the requested threshold.
double reference_weighted_peak(const std::vector<float> &stereo, unsigned fs, int weighting_us) {
    const auto c = reference_coefficients(fs, weighting_us);
    double prev[2] = {0.0, 0.0};
    double peak = 0.0;
    const std::size_t frames = stereo.size() / 2u;
    for (std::size_t n = 0; n < frames; ++n) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double x = stereo[2 * n + ch];
            const double y = c.g * x - c.g * c.r * prev[ch];
            prev[ch] = x;
            peak = std::max(peak, std::fabs(y));
        }
    }
    return peak;
}

std::vector<float> stereo_noise(unsigned seed, double amp, std::size_t frames) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(static_cast<float>(-amp), static_cast<float>(amp));
    std::vector<float> v(frames * 2u);
    for (float &x : v)
        x = dist(rng);
    return v;
}

class LegacyClipperOutput {
public:
    bool configure(unsigned sample_rate, const fmheadend::ClipperParams &params) {
        if (sample_rate < 8000 || sample_rate > 384000)
            return false;
        if (params.weighting_us != 0 && params.weighting_us != 50 && params.weighting_us != 75)
            return false;
        if (!(params.threshold_dbfs <= 0.0f && params.threshold_dbfs >= -60.0f))
            return false;

        params_ = params;
        threshold_ = std::pow(10.0f, params.threshold_dbfs / 20.0f);
        detector_.configure(sample_rate, params.weighting_us);
        return true;
    }

    void process(float *interleaved_stereo, std::size_t frames) {
        const auto coeff = detector_.coefficients();
        const float inv_g = static_cast<float>(1.0 / coeff.g);
        const float r = static_cast<float>(coeff.r);

        for (std::size_t i = 0; i < frames; ++i) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                const float input = std::isfinite(interleaved_stereo[2 * i + ch])
                    ? interleaved_stereo[2 * i + ch] : 0.0f;
                const float previous_output = detector_.previous_output(ch);
                const float x_min = r * previous_output - threshold_ * inv_g;
                const float x_max = r * previous_output + threshold_ * inv_g;
                const float output = params_.bypass ? input : std::clamp(input, x_min, x_max);
                detector_.process(ch, output);
                interleaved_stereo[2 * i + ch] = output;
            }
        }
    }

private:
    fmheadend::ClipperParams params_{};
    float threshold_ = 1.0f;
    fmheadend::PreemphasisDetector detector_{};
};

double reference_overshoot_db(double input, double previous_output, unsigned fs,
                              int weighting_us, double threshold_dbfs) {
    const auto c = reference_coefficients(fs, weighting_us);
    const double threshold = std::pow(10.0, threshold_dbfs / 20.0);
    const double ratio = std::fabs(input - c.r * previous_output) / (threshold / c.g);
    return std::max(0.0, 20.0 * std::log10(ratio));
}

void test_flat_hard_clip_weighting_off() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -6.0f;
    p.weighting_us = 0;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure weighting-us=0 hard clip test");

    auto audio = stereo_noise(1, 0.95, 20000);
    core.process(audio.data(), audio.size() / 2u);

    const float threshold = std::pow(10.0f, -6.0f / 20.0f);
    float peak = 0.0f;
    for (float x : audio)
        peak = std::max(peak, std::fabs(x));

    check(peak <= threshold + 1e-6f, "weighting-us=0 output never exceeds the flat threshold");
    check(core.clip_count() > 0, "weighting-us=0 stress signal produces clip events");
}

void test_weighted_clip_stays_under_threshold(int weighting_us) {
    constexpr unsigned fs = 48000;
    constexpr std::size_t frames = 60000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = weighting_us;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure " + std::to_string(weighting_us) + " us weighted clip test");

    auto audio = stereo_noise(static_cast<unsigned>(weighting_us) + 7u, 0.98, frames);
    core.process(audio.data(), frames);

    const double requested = std::pow(10.0, -9.0 / 20.0);
    const double reference_peak = reference_weighted_peak(audio, fs, weighting_us);

    check(reference_peak <= requested * 1.001,
          std::to_string(weighting_us) +
              " us: independently-simulated post-preemphasis peak stays within threshold");
    check(core.clip_count() > 0,
          std::to_string(weighting_us) + " us: stress signal triggers clip events");
}

void test_buffer_boundary_invariance() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = 50;

    auto source = stereo_noise(99, 0.9, 20000);
    auto whole = source;
    auto chunked = source;

    fmheadend::FmClipperCore a;
    fmheadend::FmClipperCore b;
    check(a.configure(fs, p), "configure whole-buffer clipper invariance test");
    check(b.configure(fs, p), "configure chunked-buffer clipper invariance test");

    a.process(whole.data(), whole.size() / 2u);

    std::size_t frame = 0;
    const std::size_t total = chunked.size() / 2u;
    const std::size_t chunks[] = {1, 7, 13, 64, 127, 480, 1024, 3, 29};
    std::size_t ci = 0;
    while (frame < total) {
        const std::size_t n = std::min(chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))], total - frame);
        b.process(chunked.data() + 2u * frame, n);
        frame += n;
    }

    float max_error = 0.0f;
    for (std::size_t i = 0; i < whole.size(); ++i)
        max_error = std::max(max_error, std::fabs(whole[i] - chunked[i]));
    check(max_error < 1e-7f, "clipper output is invariant to GStreamer buffer boundaries");
    check(a.clip_count() == b.clip_count(), "clip-count is identical regardless of buffer chunking");
}

void test_discont_reset() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = 50;

    fmheadend::FmClipperCore with_reset;
    fmheadend::FmClipperCore without_reset;
    check(with_reset.configure(fs, p), "configure clipper DISCONT test (reset)");
    check(without_reset.configure(fs, p), "configure clipper DISCONT test (no reset)");

    // Feed identical strong DC-like content so that internal pre-emphasis
    // filter memory (prev sample) differs measurably from zero.
    std::vector<float> warmup(500 * 2u, 0.9f);
    with_reset.process(warmup.data(), warmup.size() / 2u);
    without_reset.process(warmup.data(), warmup.size() / 2u);

    with_reset.reset();

    std::vector<float> probe_a(4 * 2u, 0.0f);
    std::vector<float> probe_b(4 * 2u, 0.0f);
    with_reset.process(probe_a.data(), probe_a.size() / 2u);
    without_reset.process(probe_b.data(), probe_b.size() / 2u);

    // After reset(), internal pre-emphasis memory is back to zero, so the
    // first output samples on a zero-input probe must be exactly zero.
    bool reset_is_flat = true;
    for (float x : probe_a)
        reset_is_flat = reset_is_flat && (x == 0.0f);
    check(reset_is_flat, "reset() clears internal pre-emphasis state on DISCONT");
}

void test_bypass_is_bit_exact() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = 50;
    p.bypass = true;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure bypass test");

    auto audio = stereo_noise(42, 0.99, 5000);
    const auto original = audio;
    core.process(audio.data(), audio.size() / 2u);

    bool identical = true;
    for (std::size_t i = 0; i < audio.size(); ++i)
        identical = identical && (audio[i] == original[i]);
    check(identical, "bypass=true leaves programme audio bit-for-bit unchanged");
    check(core.clip_count() == 0, "bypass=true never counts clip events");
}

void test_multiple_sample_rates() {
    const unsigned rates[] = {44100, 48000, 96000};
    for (unsigned fs : rates) {
        fmheadend::ClipperParams p;
        p.threshold_dbfs = -9.0f;
        p.weighting_us = 50;

        fmheadend::FmClipperCore core;
        check(core.configure(fs, p), std::to_string(fs) + " Hz: configure clipper");

        auto audio = stereo_noise(fs, 0.97, 40000);
        core.process(audio.data(), audio.size() / 2u);

        const double requested = std::pow(10.0, -9.0 / 20.0);
        const double peak = reference_weighted_peak(audio, fs, 50);
        check(peak <= requested * 1.001, std::to_string(fs) + " Hz: post-preemphasis peak stays within threshold");
    }
}

void test_stereo_independence() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = 50;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure stereo independence test");

    constexpr std::size_t frames = 20000;
    std::vector<float> audio(frames * 2u);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> loud(-0.98f, 0.98f);
    std::uniform_real_distribution<float> quiet(-0.05f, 0.05f);
    for (std::size_t n = 0; n < frames; ++n) {
        audio[2 * n] = loud(rng);      // left: heavily clipped
        audio[2 * n + 1] = quiet(rng); // right: essentially untouched
    }
    const auto original = audio;
    core.process(audio.data(), frames);

    bool right_mostly_unchanged = true;
    for (std::size_t n = 0; n < frames; ++n) {
        if (std::fabs(audio[2 * n + 1] - original[2 * n + 1]) > 1e-6f) {
            right_mostly_unchanged = false;
            break;
        }
    }
    check(right_mostly_unchanged,
          "quiet channel is left untouched while the loud channel on the same frame is clipped (no stereo link)");
}

void test_diagnostics_counters() {
    constexpr unsigned fs = 48000;
    fmheadend::ClipperParams p;
    p.threshold_dbfs = -9.0f;
    p.weighting_us = 0;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure diagnostics counter test");

    std::vector<float> silence(1000 * 2u, 0.0f);
    core.process(silence.data(), silence.size() / 2u);
    check(core.clip_count() == 0, "clip-count is zero for silence");
    check(core.max_clipping_db() == 0.0f, "max-clipping-db is zero before any clip event");

    std::vector<float> loud(1000 * 2u, 1.0f);
    core.process(loud.data(), loud.size() / 2u);
    check(core.clip_count() == 2000, "clip-count counts every clipped sample across channels");
    check(core.max_clipping_db() > 0.0f, "max-clipping-db becomes positive once a clip event occurs");

    // Diagnostics are cumulative and must survive a later quiet buffer.
    std::vector<float> quiet(500 * 2u, 0.0f);
    core.process(quiet.data(), quiet.size() / 2u);
    check(core.clip_count() == 2000, "clip-count remains a running total across subsequent buffers");
}

void test_zero_weighting_clipping_depth() {
    constexpr unsigned fs = 48000;
    constexpr float threshold_dbfs = -4.2f;
    const float input = std::pow(10.0f, 1.0f / 20.0f);

    fmheadend::ClipperParams p;
    p.threshold_dbfs = threshold_dbfs;
    p.weighting_us = 0;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure zero-weighting clipping-depth test");

    std::vector<float> audio = {input, input};
    core.process(audio.data(), 1);

    check(near(core.buffer_max_clipping_db(), 5.2, 1e-4),
          "zero weighting reports the 5.2 dB threshold overshoot");
    check(near(core.max_clipping_db(), 5.2, 1e-4),
          "zero weighting updates cumulative clipping-depth maximum");
}

void test_weighted_clipping_depth(int weighting_us) {
    constexpr unsigned fs = 48000;
    constexpr float threshold_dbfs = -4.2f;

    fmheadend::ClipperParams p;
    p.threshold_dbfs = threshold_dbfs;
    p.weighting_us = weighting_us;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p),
          "configure " + std::to_string(weighting_us) + " us clipping-depth test");

    std::vector<float> warmup = {0.1f, 0.1f};
    core.process(warmup.data(), 1);
    check(core.clip_count() == 0,
          std::to_string(weighting_us) + " us warmup does not clip");

    std::vector<float> audio = {1.0f, 1.0f};
    const double expected = reference_overshoot_db(
        audio[0], warmup[0], fs, weighting_us, threshold_dbfs);
    core.process(audio.data(), 1);

    check(near(core.buffer_max_clipping_db(), expected, 2e-4),
          std::to_string(weighting_us) +
              " us reports independently calculated weighted overshoot");
    check(near(core.max_clipping_db(), expected, 2e-4),
          std::to_string(weighting_us) +
              " us cumulative clipping depth matches weighted overshoot");
}

void test_clipping_depth_handles_near_zero_output() {
    constexpr unsigned fs = 48000;
    constexpr float threshold_dbfs = -4.2f;
    constexpr int weighting_us = 50;
    const auto c = reference_coefficients(fs, weighting_us);
    const float threshold = std::pow(10.0f, threshold_dbfs / 20.0f);
    const float half_width = static_cast<float>(threshold / c.g);
    const float target_previous_output = static_cast<float>(half_width / c.r);

    fmheadend::ClipperParams p;
    p.threshold_dbfs = threshold_dbfs;
    p.weighting_us = weighting_us;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure near-zero output clipping-depth test");

    std::vector<float> audio = {
        0.2f, 0.2f,
        target_previous_output, target_previous_output,
        -1.0f, -1.0f,
    };
    core.process(audio.data(), audio.size() / 2u);

    const double expected = reference_overshoot_db(
        -1.0, audio[2], fs, weighting_us, threshold_dbfs);
    const double old_raw_ratio_db =
        20.0 * std::log10(1.0 / std::max(std::fabs(static_cast<double>(audio[4])), 1e-12));

    check(std::fabs(audio[4]) < 1e-4f,
          "weighted clipping can produce a near-zero PCM output");
    check(near(core.buffer_max_clipping_db(), expected, 2e-4),
          "near-zero PCM output reports weighted threshold overshoot");
    check(old_raw_ratio_db > expected + 20.0,
          "legacy raw PCM ratio would substantially overstate the overshoot");
}

void test_clipping_depth_lifecycle() {
    constexpr unsigned fs = 48000;
    constexpr float threshold_dbfs = -4.2f;
    const float loud = std::pow(10.0f, 1.0f / 20.0f);

    fmheadend::ClipperParams p;
    p.threshold_dbfs = threshold_dbfs;
    p.weighting_us = 0;

    fmheadend::FmClipperCore core;
    check(core.configure(fs, p), "configure clipping-depth lifecycle test");

    std::vector<float> clipped = {loud, loud};
    core.process(clipped.data(), 1);
    const float cumulative = core.max_clipping_db();
    check(cumulative > 5.1f, "clipping-depth lifecycle establishes a cumulative maximum");

    std::vector<float> quiet = {0.0f, 0.0f};
    core.process(quiet.data(), 1);
    check(core.buffer_max_clipping_db() == 0.0f,
          "buffer clipping-depth maximum resets when the next buffer does not clip");
    check(core.max_clipping_db() == cumulative,
          "cumulative clipping-depth maximum survives a quiet buffer");

    core.reset();
    check(core.buffer_max_clipping_db() == 0.0f,
          "DISCONT-style reset clears only the buffer clipping-depth maximum");
    check(core.max_clipping_db() == cumulative,
          "DISCONT-style reset preserves cumulative clipping-depth maximum");

    check(core.configure(fs, p), "reconfigure clipping-depth lifecycle test");
    check(core.max_clipping_db() == 0.0f,
          "reconfiguration resets cumulative clipping-depth diagnostics");

    p.bypass = true;
    check(core.configure(fs, p), "configure bypass clipping-depth lifecycle test");
    std::vector<float> bypass_audio = {loud, loud};
    core.process(bypass_audio.data(), 1);
    check(core.clip_count() == 0 && core.buffer_max_clipping_db() == 0.0f &&
              core.max_clipping_db() == 0.0f,
          "bypass does not accumulate clipping diagnostics");
}

void test_telemetry_fix_preserves_pcm_output() {
    constexpr unsigned fs = 48000;
    const int weightings[] = {0, 50, 75};

    for (int weighting_us : weightings) {
        fmheadend::ClipperParams p;
        p.threshold_dbfs = -4.2f;
        p.weighting_us = weighting_us;

        auto reference_audio = stereo_noise(static_cast<unsigned>(weighting_us) + 300u, 1.2, 6000);
        auto fixed_audio = reference_audio;
        reference_audio[0] = 0.0f;
        reference_audio[1] = 0.0f;
        fixed_audio[0] = 0.0f;
        fixed_audio[1] = 0.0f;
        reference_audio[500] = 1.0f;
        fixed_audio[500] = 1.0f;
        reference_audio[501] = -1.0f;
        fixed_audio[501] = -1.0f;

        LegacyClipperOutput legacy;
        fmheadend::FmClipperCore fixed;
        check(legacy.configure(fs, p),
              "configure legacy PCM reference for " + std::to_string(weighting_us) + " us");
        check(fixed.configure(fs, p),
              "configure fixed PCM reference for " + std::to_string(weighting_us) + " us");

        const std::size_t chunks[] = {1, 7, 64, 480, 1024, 13};
        std::size_t frame = 0;
        std::size_t chunk_index = 0;
        while (frame < reference_audio.size() / 2u) {
            const std::size_t count = std::min(
                chunks[chunk_index++ % (sizeof(chunks) / sizeof(chunks[0]))],
                reference_audio.size() / 2u - frame);
            legacy.process(reference_audio.data() + 2u * frame, count);
            fixed.process(fixed_audio.data() + 2u * frame, count);
            frame += count;
        }

        bool identical = true;
        for (std::size_t i = 0; i < fixed_audio.size(); ++i)
            identical = identical && (fixed_audio[i] == reference_audio[i]);
        check(identical,
              "telemetry fix preserves bit-identical PCM for " +
                  std::to_string(weighting_us) + " us across multiple buffers");
    }
}

}  // namespace

int main() {
    test_flat_hard_clip_weighting_off();
    test_weighted_clip_stays_under_threshold(50);
    test_weighted_clip_stays_under_threshold(75);
    test_buffer_boundary_invariance();
    test_discont_reset();
    test_bypass_is_bit_exact();
    test_multiple_sample_rates();
    test_stereo_independence();
    test_diagnostics_counters();
    test_zero_weighting_clipping_depth();
    test_weighted_clipping_depth(50);
    test_weighted_clipping_depth(75);
    test_clipping_depth_handles_near_zero_output();
    test_clipping_depth_lifecycle();
    test_telemetry_fix_preserves_pcm_output();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All FM-aware clipper core tests passed.\n";
    return EXIT_SUCCESS;
}
