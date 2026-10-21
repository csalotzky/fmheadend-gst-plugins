#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "fm_limiter_core.hpp"

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

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

std::vector<float> stereo_sine(unsigned fs, double freq, double amp, std::size_t frames) {
    constexpr double pi = 3.14159265358979323846;
    std::vector<float> v(frames * 2u);
    for (std::size_t n = 0; n < frames; ++n) {
        const float x = static_cast<float>(amp * std::sin(2.0 * pi * freq * static_cast<double>(n) / static_cast<double>(fs)));
        v[2 * n] = x;
        v[2 * n + 1] = x;
    }
    return v;
}

double rms_channel(const std::vector<float> &v, std::size_t begin_frame, std::size_t end_frame, int ch) {
    double s = 0.0;
    std::size_t count = 0;
    for (std::size_t n = begin_frame; n < end_frame; ++n) {
        const double x = v[2 * n + ch];
        s += x * x;
        ++count;
    }
    return count ? std::sqrt(s / static_cast<double>(count)) : 0.0;
}

void test_coefficients() {
    const auto c50 = fmheadend::preemphasis_coefficients(48000.0, 50e-6);
    check(near(c50.r, 0.6592406302, 1e-9), "50 us r coefficient matches validated value");
    check(near(c50.g, 2.9346221663, 1e-9), "50 us g coefficient matches validated value");

    const auto c75 = fmheadend::preemphasis_coefficients(48000.0, 75e-6);
    check(near(c75.r, 0.7574651284, 1e-9), "75 us r coefficient matches validated value");
    check(near(c75.g, 4.1231184340, 1e-9), "75 us g coefficient matches validated value");
}

void test_weighting_does_not_colour_programme() {
    constexpr unsigned fs = 48000;
    fmheadend::LimiterParams p;
    p.threshold_dbfs = -1.0f;  // deliberately unreachable for this low-level test
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 50;

    fmheadend::FmLimiterCore core;
    check(core.configure(fs, p), "configure flat-path test");

    auto in = stereo_sine(fs, 10000.0, 0.01, 4000);
    const auto original = in;
    core.process(in.data(), in.size() / 2u);

    const std::size_t d = core.lookahead_samples();
    float max_error = 0.0f;
    for (std::size_t n = d; n < in.size() / 2u; ++n) {
        max_error = std::max(max_error, std::fabs(in[2 * n] - original[2 * (n - d)]));
        max_error = std::max(max_error, std::fabs(in[2 * n + 1] - original[2 * (n - d) + 1]));
    }
    check(max_error < 1e-6f, "50 us weighting is detector-only; programme path remains flat");
}

void test_impulse_is_limited_after_lookahead() {
    constexpr unsigned fs = 48000;
    fmheadend::LimiterParams p;
    p.threshold_dbfs = -9.0f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 0;

    fmheadend::FmLimiterCore core;
    check(core.configure(fs, p), "configure impulse limiter test");

    std::vector<float> audio(3000 * 2u, 0.0f);
    constexpr std::size_t impulse_at = 1000;
    audio[2 * impulse_at] = 1.0f;
    audio[2 * impulse_at + 1] = 1.0f;
    core.process(audio.data(), audio.size() / 2u);

    const std::size_t out_at = impulse_at + core.lookahead_samples();
    const float threshold = std::pow(10.0f, -9.0f / 20.0f);
    const float peak = std::max(std::fabs(audio[2 * out_at]), std::fabs(audio[2 * out_at + 1]));
    check(peak <= threshold + 2e-5f, "lookahead attack reaches threshold before triggering impulse reaches output");
    check(peak > threshold * 0.98f, "impulse is not over-attenuated by more than about 2 percent");
}

void test_hf_weighting_drives_more_gain_reduction() {
    constexpr unsigned fs = 48000;
    fmheadend::LimiterParams p;
    p.threshold_dbfs = -9.0f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 50;

    fmheadend::FmLimiterCore low_core;
    fmheadend::FmLimiterCore high_core;
    check(low_core.configure(fs, p), "configure 1 kHz weighted test");
    check(high_core.configure(fs, p), "configure 10 kHz weighted test");

    auto low = stereo_sine(fs, 1000.0, 0.30, 12000);
    auto high = stereo_sine(fs, 10000.0, 0.30, 12000);
    low_core.process(low.data(), low.size() / 2u);
    high_core.process(high.data(), high.size() / 2u);

    const std::size_t begin = 4000;
    const std::size_t end = 11000;
    const double low_rms = rms_channel(low, begin, end, 0);
    const double high_rms = rms_channel(high, begin, end, 0);

    check(low_rms > 0.19, "1 kHz at -10.46 dBFS remains largely unattenuated with 50 us detector");
    check(high_rms < low_rms * 0.65, "10 kHz receives substantially more gain reduction from 50 us detector weighting");
}

void test_stereo_link() {
    constexpr unsigned fs = 48000;
    constexpr double pi = 3.14159265358979323846;
    fmheadend::LimiterParams p;
    p.threshold_dbfs = -9.0f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 0;

    fmheadend::FmLimiterCore core;
    check(core.configure(fs, p), "configure stereo-link test");

    std::vector<float> audio(5000 * 2u, 0.0f);
    for (std::size_t n = 0; n < 5000; ++n) {
        audio[2 * n] = static_cast<float>(0.9 * std::sin(2.0 * pi * 1000.0 * static_cast<double>(n) / static_cast<double>(fs)));
        audio[2 * n + 1] = static_cast<float>(0.1 * std::sin(2.0 * pi * 1000.0 * static_cast<double>(n) / static_cast<double>(fs)));
    }
    core.process(audio.data(), audio.size() / 2u);

    const double l = rms_channel(audio, 2000, 4500, 0);
    const double r = rms_channel(audio, 2000, 4500, 1);
    const double ratio = (r > 0.0) ? l / r : 0.0;
    check(std::fabs(ratio - 9.0) < 0.15, "100 percent stereo link preserves L/R gain ratio");
}

void test_regression_vector() {
    constexpr unsigned fs = 48000;
    constexpr std::size_t frames = 24000;
    constexpr double pi = 3.14159265358979323846;

    fmheadend::LimiterParams p;
    p.threshold_dbfs = -11.5f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 1.0f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 67.0f;
    p.weighting_us = 50;

    std::vector<float> audio(frames * 2u, 0.0f);
    std::mt19937 rng(0x6d9f31u);
    std::uniform_real_distribution<float> noise(-1.2f, 1.2f);
    for (std::size_t n = 0; n < frames; ++n) {
        float left = 0.0f;
        float right = 0.0f;
        if (n == 1000 || n == 4000)
            left = (n == 1000) ? 1.0f : -1.0f;
        if (n == 2000 || n == 5000)
            right = (n == 2000) ? -1.0f : 1.0f;
        if (n >= 6000 && n < 12000) {
            const double phase = 2.0 * pi * 10000.0 *
                                 static_cast<double>(n - 6000) / static_cast<double>(fs);
            left += static_cast<float>(0.85 * std::sin(phase));
            right += static_cast<float>(0.25 * std::sin(phase));
        }
        if (n >= 12000 && n < 18000) {
            const float level = (n < 15000) ? 0.15f : 1.2f;
            left += level;
            right -= level * 0.7f;
        }
        if (n >= 18000) {
            left += noise(rng);
            right += noise(rng) * 0.55f;
        }
        audio[2 * n] = left;
        audio[2 * n + 1] = right;
    }

    fmheadend::FmLimiterCore core;
    check(core.configure(fs, p), "configure limiter regression vector");
    core.process(audio.data(), frames);

    check(std::all_of(audio.begin(), audio.end(),
                      [](float sample) { return std::isfinite(sample); }),
          "limiter regression vector produces finite output");
    check(near(core.last_detector_peak_dbfs(), 15.27248, 1e-5) &&
              near(core.last_gain_reduction_db(), 26.7047005, 1e-5),
          "limiter regression telemetry remains unchanged");
}

void test_buffer_boundary_invariance() {
    constexpr unsigned fs = 48000;
    fmheadend::LimiterParams p;
    p.threshold_dbfs = -11.5f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 50;

    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> dist(-0.8f, 0.8f);
    std::vector<float> source(25000 * 2u);
    for (float &x : source)
        x = dist(rng);

    auto whole = source;
    auto chunked = source;

    fmheadend::FmLimiterCore a;
    fmheadend::FmLimiterCore b;
    check(a.configure(fs, p), "configure whole-buffer invariance test");
    check(b.configure(fs, p), "configure chunked-buffer invariance test");

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
    check(max_error < 1e-7f, "DSP output is invariant to GStreamer buffer boundaries");
}

void test_conservative_threshold_bounds_simulated_post_preemphasis() {
    constexpr unsigned fs = 48000;
    constexpr std::size_t frames = 120000;

    fmheadend::LimiterParams p;
    p.threshold_dbfs = -11.5f;
    p.lookahead_ms = 5.0f;
    p.attack_ms = 0.25f;
    p.release_ms = 20.0f;
    p.stereo_link_percent = 100.0f;
    p.weighting_us = 50;

    fmheadend::FmLimiterCore core;
    check(core.configure(fs, p), "configure conservative post-preemphasis safety test");

    std::mt19937 rng(0x6688u);
    std::uniform_real_distribution<float> dist(-0.95f, 0.95f);
    std::vector<float> audio(frames * 2u);
    for (std::size_t n = 0; n < frames; ++n) {
        audio[2 * n] = dist(rng);
        audio[2 * n + 1] = dist(rng);
    }

    core.process(audio.data(), frames);

    fmheadend::PreemphasisDetector post;
    post.configure(fs, 50);
    float peak = 0.0f;
    for (std::size_t n = 0; n < frames; ++n) {
        peak = std::max(peak, std::fabs(post.process(0, audio[2 * n])));
        peak = std::max(peak, std::fabs(post.process(1, audio[2 * n + 1])));
    }

    const float requested = std::pow(10.0f, -9.0f / 20.0f);
    check(peak <= requested * 1.002f,
          "A -11.5 dBFS threshold keeps deterministic full-band stress signal at requested post-preemphasis ceiling");
}

}  // namespace

int main() {
    test_coefficients();
    test_weighting_does_not_colour_programme();
    test_impulse_is_limited_after_lookahead();
    test_hf_weighting_drives_more_gain_reduction();
    test_stereo_link();
    test_regression_vector();
    test_buffer_boundary_invariance();
    test_conservative_threshold_bounds_simulated_post_preemphasis();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All DSP core tests passed.\n";
    return EXIT_SUCCESS;
}
