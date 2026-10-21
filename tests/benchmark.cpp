#include "fm_limiter_core.hpp"
#include "fm_clipper_core.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
#include <cstdlib>
#include <new>

static std::size_t allocations=0;
void *operator new(std::size_t n) { ++allocations; if(auto p=std::malloc(n)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }

// Standalone benchmark: eight separate states, 480-frame blocks, no I/O in timing.
int main(int argc, char **argv) {
    constexpr unsigned frames = 48000, block = 480, streams = 8;
    std::vector<float> source(frames * 2);
    std::mt19937 rng(281);
    for (auto &x : source) x = static_cast<float>(static_cast<int>(rng() % 20001)-10000)/8000;
    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        f.read(reinterpret_cast<char *>(source.data()), source.size()*sizeof(float));
        if (f.gcount() != static_cast<std::streamsize>(source.size()*sizeof(float))) return 2;
    }
    for (int mode = 0; mode < 5; ++mode) {
        std::array<fmheadend::FmLimiterCore, streams> limiters;
        std::array<fmheadend::FmClipperCore, streams> clippers;
        fmheadend::LimiterParams lp;
        fmheadend::ClipperParams cp;
        lp.threshold_dbfs = -11.5f;
#ifdef FM_EXTENDED
        lp.true_peak_detection = mode == 1 || mode == 4;
        cp.clipping_mode = (mode == 3 || mode == 4) ? fmheadend::ClippingMode::Soft : fmheadend::ClippingMode::Hard;
#endif
        for (unsigned s=0;s<streams;++s) { limiters[s].configure(48000,lp); clippers[s].configure(48000,cp); }
        std::array<float, block*2> work;
        for (unsigned trial=0; trial<7; ++trial) {
            double seconds=0;
            double maximum_group_seconds=0;
            std::size_t processing_allocations=0;
            for (unsigned n=0;n<frames;n+=block) {
              double group_seconds=0;
              for (unsigned s=0;s<streams;++s) {
                std::memcpy(work.data(),source.data()+n*2,sizeof(work));
                const auto start=std::chrono::steady_clock::now();
                const auto before=allocations;
                if (mode < 2 || mode == 4) limiters[s].process(work.data(),block);
                if (mode >= 2) clippers[s].process(work.data(),block);
                processing_allocations+=allocations-before;
                const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                seconds+=elapsed;
                group_seconds+=elapsed;
              }
              maximum_group_seconds=std::max(maximum_group_seconds,group_seconds);
            }
            std::cout << mode << ',' << trial << ',' << seconds*1e9/(frames*streams) << ',' << seconds*100 << ',' << maximum_group_seconds*1000 << '\n';
            if (processing_allocations) { std::cerr<<"Realtime allocations: "<<processing_allocations<<'\n'; return 3; }
        }
    }
    std::cerr<<"Steady-state allocations: 0; core object bytes: limiter="<<sizeof(fmheadend::FmLimiterCore)<<", clipper="<<sizeof(fmheadend::FmClipperCore)<<'\n';
}
