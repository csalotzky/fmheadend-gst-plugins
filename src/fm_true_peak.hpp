#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "fm_preemphasis.hpp"
#if defined(__ARM_NEON) && defined(FM_TP_NEON)
#include <arm_neon.h>
#endif

namespace fmheadend {
// 4x windowed-sinc interpolation, 33 taps per phase, 16 input-frame delay.
// Weighted FIR coefficients fuse interpolation and the shared 4*Fs detector.
// The extra tap is the previous phase's one-frame history at phase zero.
class TruePeakDetector {
public:
    static constexpr unsigned delay = 16;
    static constexpr unsigned taps = 34;
    void configure(unsigned rate, int weighting) {
        constexpr double pi = 3.14159265358979323846;
        double h[4][taps]{};
        for (unsigned p=0; p<4; ++p) {
            double sum=0;
            for (unsigned k=0; k<33; ++k) {
                const double t=static_cast<double>(k)-16.0+0.25*p;
                // Continuous Blackman window centred on the fractional sample.
                const double w=std::fabs(t) <= 16 ? 0.42+0.5*std::cos(pi*t/16)+0.08*std::cos(2*pi*t/16) : 0;
                h[p][k]=(std::fabs(t)<1e-12 ? 1 : std::sin(pi*t)/(pi*t))*w;
                sum+=h[p][k];
            }
            for (auto &v : h[p]) v/=sum; // unity DC gain in every phase
        }
        const auto c=preemphasis_coefficients(4.0*rate,weighting*1e-6);
        for (unsigned k=0;k<taps;++k) for (unsigned p=0;p<4;++p) {
            const double previous=p ? h[p-1][k] : (k ? h[3][k-1] : 0);
            coeff_[k][p]=static_cast<float>(c.g*(h[p][k]-c.r*previous));
        }
        reset();
    }
    void reset() {
        history_={}; cursor_=0; previous_peak_[0]=previous_peak_[1]=0;
    }
    void process(float left, float right, float &peak_l, float &peak_r) {
        // Mirrored history avoids a wrap branch in the FIR loop.
        if (cursor_ == 0) cursor_=taps;
        --cursor_;
        history_[cursor_]=history_[cursor_+taps]={left,right};
        float l=0,r=0;
#if defined(__ARM_NEON) && defined(FM_TP_NEON)
        float32x4_t vl=vdupq_n_f32(0), vr=vl;
        for (unsigned k=0;k<taps;++k) {
            const auto c=vld1q_f32(coeff_[k].data());
            vl=vmlaq_n_f32(vl,c,history_[cursor_+k][0]);
            vr=vmlaq_n_f32(vr,c,history_[cursor_+k][1]);
        }
        l=vmaxvq_f32(vabsq_f32(vl)); r=vmaxvq_f32(vabsq_f32(vr));
#else
        float vl[4]{},vr[4]{};
        for (unsigned k=0;k<taps;++k) for (unsigned p=0;p<4;++p) {
            vl[p]+=coeff_[k][p]*history_[cursor_+k][0];
            vr[p]+=coeff_[k][p]*history_[cursor_+k][1];
        }
        for (unsigned p=0;p<4;++p) { l=std::max(l,std::fabs(vl[p])); r=std::max(r,std::fabs(vr[p])); }
#endif
        // Associate both adjacent reconstructed intervals with sample n-delay.
        peak_l=std::max(l,previous_peak_[0]); peak_r=std::max(r,previous_peak_[1]);
        previous_peak_[0]=l; previous_peak_[1]=r;
    }
private:
    alignas(16) std::array<std::array<float,4>,taps> coeff_{};
    std::array<std::array<float,2>,taps*2> history_{};
    unsigned cursor_=0;
    float previous_peak_[2]{};
};
}
