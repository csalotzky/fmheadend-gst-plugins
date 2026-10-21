#include <gst/audio/gstaudiofilter.h>
#include <gst/gst.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

// Measures the actual shared-library transform callbacks, including their
// existing mutex/map/telemetry work. Scheduling and source generation excluded.
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    constexpr unsigned frames=48000, block=480, streams=8;
    std::vector<float> source(frames*2);
    std::mt19937 rng(281);
    for(auto &x:source) x=static_cast<float>(static_cast<int>(rng()%20001)-10000)/8000;
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);
        f.read(reinterpret_cast<char*>(source.data()),source.size()*sizeof(float));
        if(f.gcount()!=static_cast<std::streamsize>(source.size()*sizeof(float))) return 2;
    }
    for(int mode=0;mode<5;++mode) {
        std::array<GstElement*,streams> limiter{},clipper{};
        GstAudioInfo info;
        gst_audio_info_init(&info);
        gst_audio_info_set_format(&info,GST_AUDIO_FORMAT_F32LE,48000,2,nullptr);
        for(unsigned s=0;s<streams;++s) {
            limiter[s]=gst_element_factory_make("fmheadendlimiter",nullptr);
            clipper[s]=gst_element_factory_make("fmheadendclipper",nullptr);
            if(!limiter[s] || !clipper[s]) return 2;
            if(g_object_class_find_property(G_OBJECT_GET_CLASS(limiter[s]),"true-peak-detection"))
                g_object_set(limiter[s],"true-peak-detection",mode==1 || mode==4,nullptr);
            if(g_object_class_find_property(G_OBJECT_GET_CLASS(clipper[s]),"clipping-mode"))
                g_object_set(clipper[s],"clipping-mode",mode==3 || mode==4 ? 1 : 0,nullptr);
            for(auto e:{limiter[s],clipper[s]}) {
                if(!GST_AUDIO_FILTER_GET_CLASS(e)->setup(GST_AUDIO_FILTER(e),&info)) return 2;
            }
        }
        auto buffer=gst_buffer_new_allocate(nullptr,block*2*sizeof(float),nullptr);
        for(unsigned trial=0;trial<7;++trial) {
            double seconds=0,maximum_group_seconds=0;
            for(unsigned n=0;n<frames;n+=block) {
                double group=0;
                for(unsigned s=0;s<streams;++s) {
                    gst_buffer_fill(buffer,0,source.data()+n*2,block*2*sizeof(float));
                    auto start=std::chrono::steady_clock::now();
                    GstFlowReturn result=GST_FLOW_OK;
                    if(mode<2 || mode==4) result=GST_BASE_TRANSFORM_GET_CLASS(limiter[s])->transform_ip(GST_BASE_TRANSFORM(limiter[s]),buffer);
                    if(mode>=2) result=GST_BASE_TRANSFORM_GET_CLASS(clipper[s])->transform_ip(GST_BASE_TRANSFORM(clipper[s]),buffer);
                    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                    if(result!=GST_FLOW_OK) return 3;
                    seconds+=elapsed; group+=elapsed;
                }
                maximum_group_seconds=std::max(maximum_group_seconds,group);
            }
            std::cout<<mode<<','<<trial<<','<<seconds*1e9/(frames*streams)<<','<<seconds*100<<','<<maximum_group_seconds*1000<<'\n';
        }
        gst_buffer_unref(buffer);
        for(auto e:limiter) gst_object_unref(e);
        for(auto e:clipper) gst_object_unref(e);
    }
}
