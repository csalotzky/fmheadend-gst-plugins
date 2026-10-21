#include <gst/audio/gstaudiofilter.h>
#include <gst/gst.h>

#include <cmath>
#include <cstdint>
#include <new>

#include "fm_limiter_core.hpp"

#ifndef PACKAGE
#define PACKAGE "fmheadend-fm-limiter"
#endif

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "FM Headend FM-aware Limiter"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/csalotzky/fmheadend-gst-plugins"
#endif

#define GST_TYPE_FMHEADEND_LIMITER (gst_fmheadend_limiter_get_type())
#define GST_FMHEADEND_LIMITER(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_FMHEADEND_LIMITER, GstFmheadendLimiter))

typedef struct _GstFmheadendLimiter GstFmheadendLimiter;
typedef struct _GstFmheadendLimiterClass GstFmheadendLimiterClass;

struct _GstFmheadendLimiter {
    GstAudioFilter parent;

    GMutex lock;
    fmheadend::FmLimiterCore *core;

    gint sample_rate;
    gboolean dirty;

    gfloat threshold_dbfs;
    gfloat lookahead_ms;
    gfloat attack_ms;
    gfloat release_ms;
    gfloat stereo_link_percent;
    gint weighting_us;
    gboolean bypass;
    gboolean true_peak_detection;

    gfloat gain_reduction_db;
    gfloat detector_peak_dbfs;
};

struct _GstFmheadendLimiterClass {
    GstAudioFilterClass parent_class;
};

G_DEFINE_TYPE(GstFmheadendLimiter, gst_fmheadend_limiter, GST_TYPE_AUDIO_FILTER)

enum {
    PROP_0,
    PROP_THRESHOLD_DBFS,
    PROP_LOOKAHEAD_MS,
    PROP_ATTACK_MS,
    PROP_RELEASE_MS,
    PROP_STEREO_LINK_PERCENT,
    PROP_WEIGHTING_US,
    PROP_BYPASS,
    PROP_GAIN_REDUCTION_DB,
    PROP_DETECTOR_PEAK_DBFS,
    PROP_LATENCY_SAMPLES,
    PROP_TRUE_PEAK_DETECTION,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = {nullptr};

static fmheadend::LimiterParams current_params(GstFmheadendLimiter *self) {
    fmheadend::LimiterParams p;
    p.threshold_dbfs = self->threshold_dbfs;
    p.lookahead_ms = self->lookahead_ms;
    p.attack_ms = self->attack_ms;
    p.release_ms = self->release_ms;
    p.stereo_link_percent = self->stereo_link_percent;
    p.weighting_us = self->weighting_us;
    p.bypass = self->bypass;
    p.true_peak_detection = self->true_peak_detection;
    return p;
}

static gboolean apply_config_locked(GstFmheadendLimiter *self) {
    if (self->sample_rate <= 0 || self->core == nullptr)
        return TRUE;

    if (!self->core->configure(static_cast<unsigned>(self->sample_rate), current_params(self))) {
        GST_ERROR_OBJECT(self, "Invalid limiter configuration");
        return FALSE;
    }

    self->dirty = FALSE;
    return TRUE;
}

static void gst_fmheadend_limiter_set_property(
    GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    auto *self = GST_FMHEADEND_LIMITER(object);

    g_mutex_lock(&self->lock);

    switch (prop_id) {
        case PROP_TRUE_PEAK_DETECTION:
            self->true_peak_detection = g_value_get_boolean(value);
            self->dirty = TRUE;
            break;
        case PROP_THRESHOLD_DBFS:
            self->threshold_dbfs = g_value_get_float(value);
            self->dirty = TRUE;
            break;
        case PROP_LOOKAHEAD_MS:
            self->lookahead_ms = g_value_get_float(value);
            self->dirty = TRUE;
            break;
        case PROP_ATTACK_MS:
            self->attack_ms = g_value_get_float(value);
            self->dirty = TRUE;
            break;
        case PROP_RELEASE_MS:
            self->release_ms = g_value_get_float(value);
            self->dirty = TRUE;
            break;
        case PROP_STEREO_LINK_PERCENT:
            self->stereo_link_percent = g_value_get_float(value);
            self->dirty = TRUE;
            break;
        case PROP_WEIGHTING_US: {
            const gint us = g_value_get_int(value);
            if (us != 0 && us != 50 && us != 75) {
                GST_WARNING_OBJECT(self, "weighting-us must be 0, 50, or 75; keeping %d", self->weighting_us);
            } else {
                self->weighting_us = us;
                self->dirty = TRUE;
            }
            break;
        }
        case PROP_BYPASS:
            self->bypass = g_value_get_boolean(value);
            self->dirty = TRUE;
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }

    g_mutex_unlock(&self->lock);
    if (prop_id == PROP_TRUE_PEAK_DETECTION || prop_id == PROP_LOOKAHEAD_MS)
        gst_element_post_message(GST_ELEMENT(self), gst_message_new_latency(GST_OBJECT(self)));
}

static void gst_fmheadend_limiter_get_property(
    GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    auto *self = GST_FMHEADEND_LIMITER(object);

    g_mutex_lock(&self->lock);

    switch (prop_id) {
        case PROP_TRUE_PEAK_DETECTION:
            g_value_set_boolean(value, self->true_peak_detection);
            break;
        case PROP_THRESHOLD_DBFS:
            g_value_set_float(value, self->threshold_dbfs);
            break;
        case PROP_LOOKAHEAD_MS:
            g_value_set_float(value, self->lookahead_ms);
            break;
        case PROP_ATTACK_MS:
            g_value_set_float(value, self->attack_ms);
            break;
        case PROP_RELEASE_MS:
            g_value_set_float(value, self->release_ms);
            break;
        case PROP_STEREO_LINK_PERCENT:
            g_value_set_float(value, self->stereo_link_percent);
            break;
        case PROP_WEIGHTING_US:
            g_value_set_int(value, self->weighting_us);
            break;
        case PROP_BYPASS:
            g_value_set_boolean(value, self->bypass);
            break;
        case PROP_GAIN_REDUCTION_DB:
            g_value_set_float(value, self->gain_reduction_db);
            break;
        case PROP_DETECTOR_PEAK_DBFS:
            g_value_set_float(value, self->detector_peak_dbfs);
            break;
        case PROP_LATENCY_SAMPLES: {
            guint samples = 0;
            if (self->sample_rate > 0) {
                samples = static_cast<guint>(std::llround(
                    self->lookahead_ms * 1e-3 * static_cast<double>(self->sample_rate)));
                if (self->true_peak_detection) samples += fmheadend::TruePeakDetector::delay;
            }
            g_value_set_uint(value, samples);
            break;
        }
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }

    g_mutex_unlock(&self->lock);
}

static void gst_fmheadend_limiter_finalize(GObject *object) {
    auto *self = GST_FMHEADEND_LIMITER(object);
    delete self->core;
    self->core = nullptr;
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_fmheadend_limiter_parent_class)->finalize(object);
}

static gboolean gst_fmheadend_limiter_setup(GstAudioFilter *filter, const GstAudioInfo *info) {
    auto *self = GST_FMHEADEND_LIMITER(filter);

    if (GST_AUDIO_INFO_FORMAT(info) != GST_AUDIO_FORMAT_F32LE ||
        GST_AUDIO_INFO_LAYOUT(info) != GST_AUDIO_LAYOUT_INTERLEAVED ||
        GST_AUDIO_INFO_CHANNELS(info) != 2) {
        GST_ERROR_OBJECT(self, "Only interleaved stereo F32LE is supported");
        return FALSE;
    }

    g_mutex_lock(&self->lock);
    self->sample_rate = GST_AUDIO_INFO_RATE(info);
    self->dirty = TRUE;
    const gboolean ok = apply_config_locked(self);
    g_mutex_unlock(&self->lock);

    if (ok) {
        GST_INFO_OBJECT(self,
            "configured: %d Hz, threshold %.2f dBFS, lookahead %.3f ms, attack %.3f ms, "
            "release %.3f ms, link %.1f%%, weighting %d us",
            self->sample_rate, self->threshold_dbfs, self->lookahead_ms, self->attack_ms,
            self->release_ms, self->stereo_link_percent, self->weighting_us);
    }
    return ok;
}

static GstFlowReturn gst_fmheadend_limiter_transform_ip(GstBaseTransform *base, GstBuffer *buffer) {
    auto *self = GST_FMHEADEND_LIMITER(base);

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) {
        GST_ERROR_OBJECT(self, "Could not map audio buffer");
        return GST_FLOW_ERROR;
    }

    GstFlowReturn result = GST_FLOW_OK;
    g_mutex_lock(&self->lock);

    do {
        if (self->dirty && !apply_config_locked(self)) {
            result = GST_FLOW_ERROR;
            break;
        }

        if (self->core == nullptr || self->sample_rate <= 0) {
            GST_ERROR_OBJECT(self, "Limiter core is not configured");
            result = GST_FLOW_NOT_NEGOTIATED;
            break;
        }

        if ((map.size % (sizeof(float) * 2u)) != 0u) {
            GST_ERROR_OBJECT(self, "Invalid stereo F32 buffer size: %" G_GSIZE_FORMAT, map.size);
            result = GST_FLOW_ERROR;
            break;
        }

        if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
            self->core->reset();

        auto *samples = reinterpret_cast<float *>(map.data);
        const std::size_t frames = map.size / (sizeof(float) * 2u);
        self->core->process(samples, frames);

        self->gain_reduction_db = self->core->last_gain_reduction_db();
        self->detector_peak_dbfs = self->core->last_detector_peak_dbfs();
    } while (false);

    g_mutex_unlock(&self->lock);
    gst_buffer_unmap(buffer, &map);
    return result;
}

static gboolean gst_fmheadend_limiter_query(
    GstBaseTransform *base, GstPadDirection direction, GstQuery *query) {
    auto *self = GST_FMHEADEND_LIMITER(base);

    if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
        g_mutex_lock(&self->lock);
        const gint rate = self->sample_rate;
        const gfloat lookahead_ms = self->lookahead_ms;
        const gboolean true_peak = self->true_peak_detection;
        g_mutex_unlock(&self->lock);

        if (rate <= 0)
            return FALSE;

        if (!gst_pad_peer_query(GST_BASE_TRANSFORM(self)->sinkpad, query))
            return FALSE;

        gboolean live = FALSE;
        GstClockTime min_latency = 0;
        GstClockTime max_latency = GST_CLOCK_TIME_NONE;
        gst_query_parse_latency(query, &live, &min_latency, &max_latency);

        const guint samples = static_cast<guint>(std::llround(
            lookahead_ms * 1e-3 * static_cast<double>(rate))) +
            (true_peak ? fmheadend::TruePeakDetector::delay : 0u);
        const GstClockTime own_latency =
            gst_util_uint64_scale_round(samples, GST_SECOND, rate);

        min_latency += own_latency;
        if (max_latency != GST_CLOCK_TIME_NONE)
            max_latency += own_latency;
        gst_query_set_latency(query, live, min_latency, max_latency);
        return TRUE;
    }

    auto *parent = GST_BASE_TRANSFORM_CLASS(gst_fmheadend_limiter_parent_class);
    return parent->query ? parent->query(base, direction, query) : FALSE;
}

static void gst_fmheadend_limiter_class_init(GstFmheadendLimiterClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);
    auto *audio_class = GST_AUDIO_FILTER_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_fmheadend_limiter_set_property;
    gobject_class->get_property = gst_fmheadend_limiter_get_property;
    gobject_class->finalize = gst_fmheadend_limiter_finalize;

    gst_element_class_set_static_metadata(
        element_class,
        "FM-aware Lookahead Limiter",
        "Filter/Effect/Audio",
        "Stereo lookahead limiter with a 0/50/75 us FM-preemphasis-weighted detector and flat programme path",
        "FMheadend project contributors"
    );

    GstCaps *caps = gst_caps_from_string(
        "audio/x-raw,format=(string)F32LE,layout=(string)interleaved,"
        "channels=(int)2,rate=(int)[8000,192000]"
    );
    gst_audio_filter_class_add_pad_templates(audio_class, caps);
    gst_caps_unref(caps);

    audio_class->setup = GST_DEBUG_FUNCPTR(gst_fmheadend_limiter_setup);
    base_class->transform_ip = GST_DEBUG_FUNCPTR(gst_fmheadend_limiter_transform_ip);
    base_class->query = GST_DEBUG_FUNCPTR(gst_fmheadend_limiter_query);

    properties[PROP_THRESHOLD_DBFS] = g_param_spec_float(
        "threshold-dbfs", "Threshold", "Detector threshold in dBFS",
        -60.0f, 0.0f, -9.0f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_LOOKAHEAD_MS] = g_param_spec_float(
        "lookahead-ms", "Lookahead", "Programme-path lookahead delay in milliseconds",
        0.0f, 20.0f, 5.0f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_ATTACK_MS] = g_param_spec_float(
        "attack-ms", "Attack", "Gain-ramp attack time in milliseconds; internally limited to lookahead",
        0.0f, 20.0f, 0.25f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_RELEASE_MS] = g_param_spec_float(
        "release-ms", "Release", "Time for 90 percent recovery toward unity in milliseconds",
        1.0f, 1000.0f, 20.0f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_STEREO_LINK_PERCENT] = g_param_spec_float(
        "stereo-link-percent", "Stereo link", "Stereo detector gain-link percentage",
        0.0f, 100.0f, 100.0f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_WEIGHTING_US] = g_param_spec_int(
        "weighting-us", "FM weighting", "Detector preemphasis time constant: 0, 50, or 75 us",
        0, 75, 50,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_BYPASS] = g_param_spec_boolean(
        "bypass", "Bypass", "Disable gain reduction while retaining lookahead latency",
        FALSE,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_GAIN_REDUCTION_DB] = g_param_spec_float(
        "gain-reduction-db", "Gain reduction", "Most recent linked gain reduction in dB",
        0.0f, 120.0f, 0.0f,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_DETECTOR_PEAK_DBFS] = g_param_spec_float(
        "detector-peak-dbfs", "Detector peak", "Most recent weighted detector peak in dBFS",
        -200.0f, 100.0f, -200.0f,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_LATENCY_SAMPLES] = g_param_spec_uint(
        "latency-samples", "Latency", "Current programme-path latency in samples",
        0, G_MAXUINT, 0,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_TRUE_PEAK_DETECTION] = g_param_spec_boolean(
        "true-peak-detection", "True peak detection",
        "4x FIR preemphasis-weighted sidechain; adds 16 sample frames of latency",
        FALSE, static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_properties(gobject_class, N_PROPERTIES, properties);
}

static void gst_fmheadend_limiter_init(GstFmheadendLimiter *self) {
    g_mutex_init(&self->lock);
    self->core = new (std::nothrow) fmheadend::FmLimiterCore();
    if (self->core == nullptr)
        g_error("Could not allocate FM limiter DSP core");

    self->sample_rate = 0;
    self->dirty = TRUE;

    self->threshold_dbfs = -9.0f;
    self->lookahead_ms = 5.0f;
    self->attack_ms = 0.25f;
    self->release_ms = 20.0f;
    self->stereo_link_percent = 100.0f;
    self->weighting_us = 50;
    self->bypass = FALSE;
    self->true_peak_detection = FALSE;

    self->gain_reduction_db = 0.0f;
    self->detector_peak_dbfs = -200.0f;

    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_qos_enabled(GST_BASE_TRANSFORM(self), TRUE);
}

static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(
        plugin, "fmheadendlimiter", GST_RANK_NONE, GST_TYPE_FMHEADEND_LIMITER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    fmheadendlimiter,
    "FM-aware lookahead limiter for FM headend processing",
    plugin_init,
    "0.1.0",
    "MIT",
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN
)
