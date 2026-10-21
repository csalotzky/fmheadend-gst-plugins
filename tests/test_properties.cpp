#include <gst/gst.h>
#include <cmath>
#include <iostream>
#include <cstdlib>
static void check(bool ok,const char *what) { if(!ok) { std::cerr<<what<<'\n'; std::exit(1); } }
int main(int argc,char **argv) {
    gst_init(&argc,&argv);
    GstElement *l=gst_element_factory_make("fmheadendlimiter",nullptr), *c=gst_element_factory_make("fmheadendclipper",nullptr);
    check(l && c,"plugin discovery");
    auto tp=g_object_class_find_property(G_OBJECT_GET_CLASS(l),"true-peak-detection");
    check(tp && G_IS_PARAM_SPEC_BOOLEAN(tp) && !G_PARAM_SPEC_BOOLEAN(tp)->default_value,"true peak boolean/default");
    auto mode=g_object_class_find_property(G_OBJECT_GET_CLASS(c),"clipping-mode");
    check(mode && G_IS_PARAM_SPEC_ENUM(mode) && G_PARAM_SPEC_ENUM(mode)->default_value==0,"enum metadata");
    auto ec=G_PARAM_SPEC_ENUM(mode)->enum_class;
    check(g_enum_get_value_by_nick(ec,"hard")->value==0 && g_enum_get_value_by_nick(ec,"soft")->value==1,"enum nicks");
    auto knee=g_object_class_find_property(G_OBJECT_GET_CLASS(c),"soft-knee-db");
    check(knee && G_IS_PARAM_SPEC_FLOAT(knee) && G_PARAM_SPEC_FLOAT(knee)->minimum==0 && G_PARAM_SPEC_FLOAT(knee)->maximum==6 && G_PARAM_SPEC_FLOAT(knee)->default_value==2,"knee metadata");
    auto hf=g_object_class_find_property(G_OBJECT_GET_CLASS(c),"hf-correction-us");
    check(hf && G_IS_PARAM_SPEC_INT(hf) && G_PARAM_SPEC_INT(hf)->minimum==-10 && G_PARAM_SPEC_INT(hf)->maximum==30 && G_PARAM_SPEC_INT(hf)->default_value==0,"HF correction metadata");
    g_object_set(l,"true-peak-detection",TRUE,nullptr);
    g_object_set(c,"clipping-mode",1,"soft-knee-db",3.0f,"hf-correction-us",5,nullptr);
    gboolean enabled=FALSE; gint m=0, correction=0; gfloat k=0;
    g_object_get(l,"true-peak-detection",&enabled,nullptr); g_object_get(c,"clipping-mode",&m,"soft-knee-db",&k,"hf-correction-us",&correction,nullptr);
    check(enabled && m==1 && k==3 && correction==5,"readback"); gst_object_unref(l); gst_object_unref(c);
    for(bool on:{false,true}) {
        GError *err=nullptr;
        auto p=gst_parse_launch("audiotestsrc num-buffers=100 samplesperbuffer=480 is-live=true ! audioconvert ! audio/x-raw,format=F32LE,rate=48000,channels=2 ! fmheadendlimiter name=l ! fmheadendclipper clipping-mode=soft ! fakesink sync=false",&err);
        check(p && !err,"parse pipeline"); l=gst_bin_get_by_name(GST_BIN(p),"l"); g_object_set(l,"true-peak-detection",on,nullptr);
        gst_element_set_state(p,GST_STATE_PLAYING); gst_element_get_state(p,nullptr,nullptr,2*GST_SECOND);
        guint frames=0; g_object_get(l,"latency-samples",&frames,nullptr); check(frames==(on?256u:240u),"latency property");
        auto q=gst_query_new_latency(); check(gst_element_query(l,q),"latency query");
        gboolean live; GstClockTime min,max; gst_query_parse_latency(q,&live,&min,&max);
        check(live && min==gst_util_uint64_scale_round(480+frames,GST_SECOND,48000),"reported latency includes FIR delay");
        gst_query_unref(q);
        auto bus=gst_element_get_bus(p); auto msg=gst_bus_timed_pop_filtered(bus,5*GST_SECOND,static_cast<GstMessageType>(GST_MESSAGE_ERROR|GST_MESSAGE_EOS));
        check(msg && GST_MESSAGE_TYPE(msg)==GST_MESSAGE_EOS,"pipeline execution");
        gst_message_unref(msg); gst_object_unref(bus); gst_element_set_state(p,GST_STATE_NULL); gst_object_unref(l); gst_object_unref(p);
    }
    std::cout<<"Property/readback/latency integration passed\n";
    // An ignored soft-knee property must not reset hard-mode feedback/counters.
    auto pipe=gst_parse_launch("appsrc name=source format=time caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! fmheadendclipper name=clip ! appsink name=sink sync=false",nullptr);
    check(pipe,"appsrc integration pipeline");
    auto source=gst_bin_get_by_name(GST_BIN(pipe),"source"), sink=gst_bin_get_by_name(GST_BIN(pipe),"sink");
    c=gst_bin_get_by_name(GST_BIN(pipe),"clip"); gst_element_set_state(pipe,GST_STATE_PLAYING);
    for(unsigned n=0;n<2;++n) {
        if(n) g_object_set(c,"soft-knee-db",5.0f,nullptr);
        float samples[]={1,1}; auto buffer=gst_buffer_new_allocate(nullptr,sizeof(samples),nullptr);
        gst_buffer_fill(buffer,0,samples,sizeof(samples));
        GST_BUFFER_PTS(buffer)=gst_util_uint64_scale(n,GST_SECOND,48000);
        GST_BUFFER_DURATION(buffer)=gst_util_uint64_scale(1,GST_SECOND,48000);
        GstFlowReturn flow; g_signal_emit_by_name(source,"push-buffer",buffer,&flow); gst_buffer_unref(buffer); check(flow==GST_FLOW_OK,"push buffer");
        GstSample *sample=nullptr; g_signal_emit_by_name(sink,"try-pull-sample",GST_SECOND,&sample); check(sample,"pull buffer"); gst_sample_unref(sample);
        guint64 count=0; g_object_get(c,"clip-count",&count,nullptr); check(count==2*(n+1),"inactive knee preserves hard history/counters");
    }
    gst_element_set_state(pipe,GST_STATE_NULL); gst_object_unref(c); gst_object_unref(source); gst_object_unref(sink); gst_object_unref(pipe);
    std::cout<<"Inactive soft-knee property test passed\n";
}
