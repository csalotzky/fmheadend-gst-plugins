#include "fm_limiter_core.hpp"
#include "fm_clipper_core.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
// Raw stereo F32LE input/output; used only by offline independent validation.
int main(int argc,char **argv) {
    if(argc!=7) return 2;
    const int mode=std::atoi(argv[3]), weighting=std::atoi(argv[4]);
    fmheadend::LimiterParams p; p.weighting_us=weighting; p.threshold_dbfs=std::stof(argv[5]); p.attack_ms=std::stof(argv[6]);
    p.true_peak_detection=mode==1;
    fmheadend::ClipperParams q; q.weighting_us=weighting; q.clipping_mode=mode==3 ? fmheadend::ClippingMode::Soft : fmheadend::ClippingMode::Hard;
    std::ifstream f(argv[1],std::ios::binary|std::ios::ate); auto bytes=f.tellg(); if(bytes<0 || bytes%8) return 2;
    std::vector<float> x(static_cast<std::size_t>(bytes)/4); f.seekg(0); f.read(reinterpret_cast<char*>(x.data()),bytes);
    fmheadend::FmLimiterCore l; fmheadend::FmClipperCore c; l.configure(48000,p); c.configure(48000,q);
    fmheadend::TruePeakDetector detector; detector.configure(48000,weighting);
    for(std::size_t n=0;n<x.size()/2;n+=480) {
        auto count=std::min<std::size_t>(480,x.size()/2-n);
        if(mode==4) for(std::size_t i=n;i<n+count;++i) detector.process(x[2*i],x[2*i+1],x[2*i],x[2*i+1]);
        else if(mode<2) l.process(x.data()+2*n,count);
        else c.process(x.data()+2*n,count);
    }
    std::ofstream o(argv[2],std::ios::binary); o.write(reinterpret_cast<const char*>(x.data()),bytes);
}
