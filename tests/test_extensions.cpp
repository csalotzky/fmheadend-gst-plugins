#include "fm_limiter_core.hpp"
#include "fm_clipper_core.hpp"
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <random>
#include <limits>
#ifdef FM_LEGACY_COMPARE
#define fmheadend legacy
#include "../baseline/fm_limiter_core.hpp"
#include "../baseline/fm_clipper_core.hpp"
#undef fmheadend
#endif

static unsigned failures=0;
static void check(bool ok,const char *name) { if (!ok) { ++failures; std::cerr<<"FAIL "<<name<<'\n'; } }
static void same(const std::vector<float>&a,const std::vector<float>&b,const char *name) {
    check(a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0,name);
}
template<class Core> void chunks(Core &c,std::vector<float>&x) {
    const unsigned sizes[]={1,7,128,3,480,1024};
    std::size_t n=0,i=0;
    while(n<x.size()/2) { auto count=std::min<std::size_t>(sizes[i++%6],x.size()/2-n); c.process(x.data()+2*n,count); n+=count; }
}
int main() {
    std::mt19937 rng(5678);
    std::vector<float> input(4096*2);
    for(auto &x:input) x=static_cast<float>(static_cast<int>(rng()%20001)-10000)/9000;
    input[0]=std::numeric_limits<float>::quiet_NaN(); input[1]=std::numeric_limits<float>::infinity();
    for(unsigned rate:{8000u,44100u,48000u,96000u,192000u,384000u}) for(int w:{0,50,75}) {
        for(float link:{0.f,37.f,100.f}) for(float look:{0.f,0.25f,5.f}) for(bool tp:{false,true}) {
            fmheadend::LimiterParams p; p.weighting_us=w; p.stereo_link_percent=link;
            p.lookahead_ms=look; p.attack_ms=look==5 ? 1.f : 0.f; p.release_ms=look==0 ? 1.f : 20.f;
            p.threshold_dbfs=link==37 ? -11.5f : -9.f; p.true_peak_detection=tp;
            fmheadend::FmLimiterCore a,b;
            check(a.configure(rate,p)&&b.configure(rate,p),"limiter configure");
            auto whole=input,part=input; a.process(whole.data(),whole.size()/2); chunks(b,part); same(whole,part,"limiter partitions");
            for(auto x:whole) check(std::isfinite(x),"limiter finite");
            a.reset(); auto reset=input; chunks(a,reset); same(reset,whole,"limiter reset");
            a.configure(rate==48000?44100:48000,p); a.configure(rate,p);
            reset=input; chunks(a,reset); same(reset,whole,"rate reconfiguration resets limiter");
#ifdef FM_LEGACY_COMPARE
            if(!tp) {
                legacy::LimiterParams old;
                old.weighting_us=w; old.stereo_link_percent=link; old.lookahead_ms=p.lookahead_ms;
                old.attack_ms=p.attack_ms; old.release_ms=p.release_ms; old.threshold_dbfs=p.threshold_dbfs;
                legacy::FmLimiterCore ref; ref.configure(rate,old); auto expected=input; chunks(ref,expected);
                same(whole,expected,"legacy limiter bit exact");
            }
#endif
            p.bypass=true; a.configure(rate,p); auto out=input; chunks(a,out);
            const unsigned delay=a.lookahead_samples();
            check(delay==static_cast<unsigned>(std::llround(look*.001*rate))+(tp?16u:0u),"limiter latency");
            for(std::size_t n=0;n<out.size()/2;++n) for(unsigned ch=0;ch<2;++ch) {
                float expected=n<delay ? 0 : input[2*(n-delay)+ch]; if(!std::isfinite(expected)) expected=0;
                check(out[2*n+ch]==expected,"bypass alignment");
            }
        }
        for(float threshold:{-60.f,-9.f,0.f}) for(float knee:{0.f,0.001f,2.f,6.f}) {
            fmheadend::ClipperParams p; p.weighting_us=w; p.threshold_dbfs=threshold;
            p.soft_knee_db=knee; p.clipping_mode=fmheadend::ClippingMode::Soft;
            fmheadend::FmClipperCore a,b; a.configure(rate,p); b.configure(rate,p);
            auto whole=input,part=input; a.process(whole.data(),whole.size()/2); chunks(b,part); same(whole,part,"clipper partitions");
            check(a.clip_count()==b.clip_count(),"clip count partitions");
            a.reset(); auto again=input; chunks(a,again); same(whole,again,"soft reset");
            a.configure(rate==48000?44100:48000,p); a.configure(rate,p);
            again=input; chunks(a,again); same(whole,again,"rate reconfiguration resets clipper");
            const double r=w ? std::exp(-1.0/(rate*w*1e-6)) : 0, g=1/(1-r);
            double prev[2]{}; const double t=a.threshold_linear();
            for(std::size_t n=0;n<whole.size()/2;++n) for(unsigned ch=0;ch<2;++ch) {
                const float sample=input[2*n+ch];
                const double candidate=g*((std::isfinite(sample)?sample:0)-r*prev[ch]);
                const double out=whole[2*n+ch], y=g*(out-r*prev[ch]); prev[ch]=out;
                check(std::isfinite(out)&&std::fabs(y)<=t*(1+4e-6),"soft weighted ceiling");
                check(std::fabs(y)<=std::fabs(candidate)+t*4e-6,"no weighted gain increase");
                if(std::fabs(candidate)<t*std::pow(10.,-knee/20.)*.9999)
                    check(out==(std::isfinite(sample)?sample:0),"weighted identity below knee");
            }
            b.configure(rate,p); auto negative=input;
            for(auto &x:negative) x=-x;
            chunks(b,negative);
            for(std::size_t i=0;i<whole.size();++i) check(negative[i]==-whole[i],"weighted odd symmetry");
            if(knee==0) {
                p.clipping_mode=fmheadend::ClippingMode::Hard; b.configure(rate,p); part=input; chunks(b,part); same(whole,part,"zero knee equals hard");
#ifdef FM_LEGACY_COMPARE
                legacy::ClipperParams old; old.weighting_us=w; old.threshold_dbfs=threshold;
                legacy::FmClipperCore ref; ref.configure(rate,old); auto expected=input; chunks(ref,expected); same(whole,expected,"legacy clipper bit exact");
#endif
            }
            p.bypass=true; a.configure(rate,p); part=input; chunks(a,part);
            for(std::size_t i=0;i<part.size();++i) check(part[i]==(std::isfinite(input[i])?input[i]:0),"clip bypass");
        }
    }
    for(bool tp:{false,true}) for(float link:{0.f,37.f,100.f}) {
        fmheadend::LimiterParams p; p.weighting_us=0; p.true_peak_detection=tp;
        p.stereo_link_percent=link; p.threshold_dbfs=-11.5f;
        fmheadend::FmLimiterCore c; c.configure(48000,p);
        std::vector<float> signal(8192*2);
        for(std::size_t i=0;i<signal.size();i+=2) { signal[i]=.8f; signal[i+1]=.08f; }
        chunks(c,signal);
        const float t=c.threshold_linear();
        check(std::fabs(signal[signal.size()-2]-t)<1e-5f,"true-peak threshold reaches expected gain");
        const float right=.08f*(1.f-link*.01f*(1.f-t/.8f));
        check(std::fabs(signal.back()-right)<1e-5f,"true-peak stereo-link semantics");
    }
    // Transfer, symmetry, monotonicity, identity, C1 joins and knee-only telemetry.
    for(float knee:{0.001f,2.f,6.f}) {
        fmheadend::ClipperParams p; p.weighting_us=0; p.threshold_dbfs=0; p.soft_knee_db=knee; p.clipping_mode=fmheadend::ClippingMode::Soft;
        fmheadend::FmClipperCore c; c.configure(48000,p);
        const double a=std::pow(10.0,-knee/20.0), d=1-a;
        float previous=0;
        for(unsigned i=0;i<10000;++i) {
            const float u=static_cast<float>(2.*i/9999); float x[]={u,-u}; c.process(x,1);
            check(x[0]>=previous && x[0]<=u && x[0]<=1 && x[0]==-x[1],"transfer monotone odd ceiling");
            if(u<a) check(x[0]==u,"identity below knee");
            previous=x[0];
        }
        if(knee>=2) for(double boundary:{a,a+2*d}) {
            const double eps=0.001;
            float x[]={static_cast<float>(boundary-eps),0,static_cast<float>(boundary),0,static_cast<float>(boundary+eps),0}; c.process(x,3);
            check(std::fabs((x[2]-x[0])-(x[4]-x[2]))/eps<0.015,"C1 knee boundary");
        }
        c.configure(48000,p); float x[]={static_cast<float>((a+1)/2),0}; c.process(x,1);
        check(c.clip_count()==1 && c.buffer_max_clipping_db()==0,"knee telemetry is not overshoot");
    }
    std::cout<<"Extension failures: "<<failures<<'\n'; return failures?1:0;
}
