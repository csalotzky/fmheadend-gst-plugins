#include <gst/audio/gstaudiofilter.h>
#include <gst/gst.h>

#include <cstdint>
#include <new>

#include "fm_clipper_core.hpp"

#ifndef PACKAGE
#define PACKAGE "fmheadend-fm-clipper"
#endif

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "FM Headend FM-aware Clipper"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/csalotzky/fmheadend-gst-plugins"
#endif

#define GST_TYPE_FMHEADEND_CLIPPER (gst_fmheadend_clipper_get_type())
#define GST_FMHEADEND_CLIPPER(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_FMHEADEND_CLIPPER, GstFmheadendClipper))

typedef struct _GstFmheadendClipper GstFmheadendClipper;
typedef struct _GstFmheadendClipperClass GstFmheadendClipperClass;

struct _GstFmheadendClipper {
    GstAudioFilter parent;

    GMutex lock;
    fmheadend::FmClipperCore *core;

    gint sample_rate;
    gboolean dirty;

    gfloat threshold_dbfs;
    gint weighting_us;
    gboolean bypass;
    gint clipping_mode;
    gfloat soft_knee_db;
    gint hf_correction_us;

    gfloat detector_peak_dbfs;
};

struct _GstFmheadendClipperClass {
    GstAudioFilterClass parent_class;
};

G_DEFINE_TYPE(GstFmheadendClipper, gst_fmheadend_clipper, GST_TYPE_AUDIO_FILTER)

enum {
    PROP_0,
    PROP_THRESHOLD_DBFS,
    PROP_WEIGHTING_US,
    PROP_BYPASS,
    PROP_DETECTOR_PEAK_DBFS,
    PROP_CLIP_COUNT,
    PROP_MAX_CLIPPING_DB,
    PROP_BUFFER_MAX_CLIPPING_DB,
    PROP_CLIPPING_MODE,
    PROP_SOFT_KNEE_DB,
    PROP_HF_CORRECTION_US,
    N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = {nullptr};

static GType clipping_mode_type() {
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            {0, "Hard clipping", "hard"}, {1, "Soft knee with hard ceiling", "soft"},
            {0, nullptr, nullptr}
        };
        g_once_init_leave(&type, g_enum_register_static("GstFmheadendClippingMode", values));
    }
    return static_cast<GType>(type);
}

static fmheadend::ClipperParams current_params(GstFmheadendClipper *self) {
    fmheadend::ClipperParams p;
    p.threshold_dbfs = self->threshold_dbfs;
    p.weighting_us = self->weighting_us;
    p.bypass = self->bypass;
    p.clipping_mode = static_cast<fmheadend::ClippingMode>(self->clipping_mode);
    p.soft_knee_db = self->soft_knee_db;
    p.hf_correction_us = self->hf_correction_us;
    return p;
}

static gboolean apply_config_locked(GstFmheadendClipper *self) {
    if (self->sample_rate <= 0 || self->core == nullptr)
        return TRUE;

    if (!self->core->configure(static_cast<unsigned>(self->sample_rate), current_params(self))) {
        GST_ERROR_OBJECT(self, "Invalid clipper configuration");
        return FALSE;
    }

    self->dirty = FALSE;
    return TRUE;
}

static void gst_fmheadend_clipper_set_property(
    GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    auto *self = GST_FMHEADEND_CLIPPER(object);

    g_mutex_lock(&self->lock);

    switch (prop_id) {
        case PROP_CLIPPING_MODE:
            self->clipping_mode = g_value_get_enum(value);
            self->dirty = TRUE;
            break;
        case PROP_HF_CORRECTION_US:
            self->hf_correction_us = g_value_get_int(value);
            self->dirty = TRUE;
            break;
        case PROP_SOFT_KNEE_DB:
            self->soft_knee_db = g_value_get_float(value);
            // An inactive knee setting must not reset the hard-mode history.
            if (self->clipping_mode == 1) self->dirty = TRUE;
            break;
        case PROP_THRESHOLD_DBFS:
            self->threshold_dbfs = g_value_get_float(value);
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
}

static void gst_fmheadend_clipper_get_property(
    GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    auto *self = GST_FMHEADEND_CLIPPER(object);

    g_mutex_lock(&self->lock);

    switch (prop_id) {
        case PROP_CLIPPING_MODE:
            g_value_set_enum(value, self->clipping_mode);
            break;
        case PROP_HF_CORRECTION_US:
            g_value_set_int(value, self->hf_correction_us);
            break;
        case PROP_SOFT_KNEE_DB:
            g_value_set_float(value, self->soft_knee_db);
            break;
        case PROP_THRESHOLD_DBFS:
            g_value_set_float(value, self->threshold_dbfs);
            break;
        case PROP_WEIGHTING_US:
            g_value_set_int(value, self->weighting_us);
            break;
        case PROP_BYPASS:
            g_value_set_boolean(value, self->bypass);
            break;
        case PROP_DETECTOR_PEAK_DBFS:
            g_value_set_float(value, self->detector_peak_dbfs);
            break;
        case PROP_CLIP_COUNT:
            g_value_set_uint64(value, self->core ? self->core->clip_count() : 0);
            break;
        case PROP_MAX_CLIPPING_DB:
            g_value_set_float(value, self->core ? self->core->max_clipping_db() : 0.0f);
            break;
        case PROP_BUFFER_MAX_CLIPPING_DB:
            g_value_set_float(value, self->core ? self->core->buffer_max_clipping_db() : 0.0f);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }

    g_mutex_unlock(&self->lock);
}

static void gst_fmheadend_clipper_finalize(GObject *object) {
    auto *self = GST_FMHEADEND_CLIPPER(object);
    delete self->core;
    self->core = nullptr;
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_fmheadend_clipper_parent_class)->finalize(object);
}

static gboolean gst_fmheadend_clipper_setup(GstAudioFilter *filter, const GstAudioInfo *info) {
    auto *self = GST_FMHEADEND_CLIPPER(filter);

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
            "configured: %d Hz, threshold %.2f dBFS, weighting %d us, bypass %d",
            self->sample_rate, self->threshold_dbfs, self->weighting_us, self->bypass);
    }
    return ok;
}

static GstFlowReturn gst_fmheadend_clipper_transform_ip(GstBaseTransform *base, GstBuffer *buffer) {
    auto *self = GST_FMHEADEND_CLIPPER(base);

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
            GST_ERROR_OBJECT(self, "Clipper core is not configured");
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

        self->detector_peak_dbfs = self->core->last_detector_peak_dbfs();
    } while (false);

    g_mutex_unlock(&self->lock);
    gst_buffer_unmap(buffer, &map);
    return result;
}

static void gst_fmheadend_clipper_class_init(GstFmheadendClipperClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);
    auto *audio_class = GST_AUDIO_FILTER_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = gst_fmheadend_clipper_set_property;
    gobject_class->get_property = gst_fmheadend_clipper_get_property;
    gobject_class->finalize = gst_fmheadend_clipper_finalize;

    gst_element_class_set_static_metadata(
        element_class,
        "FM-aware Clipper",
        "Filter/Effect/Audio",
        "Zero-latency stereo hard/soft clipper constraining a 0/50/75 us FM-preemphasis-weighted "
        "estimate while keeping a flat programme path; last-resort protection against residual "
        "peaks that survive an upstream limiter and low-pass filter",
        "FMheadend project contributors"
    );

    GstCaps *caps = gst_caps_from_string(
        "audio/x-raw,format=(string)F32LE,layout=(string)interleaved,"
        "channels=(int)2,rate=(int)[8000,192000]"
    );
    gst_audio_filter_class_add_pad_templates(audio_class, caps);
    gst_caps_unref(caps);

    audio_class->setup = GST_DEBUG_FUNCPTR(gst_fmheadend_clipper_setup);
    base_class->transform_ip = GST_DEBUG_FUNCPTR(gst_fmheadend_clipper_transform_ip);
    // No latency query override: the clipper has zero algorithmic latency,
    // so the default GstBaseTransform passthrough behaviour is correct.

    properties[PROP_THRESHOLD_DBFS] = g_param_spec_float(
        "threshold-dbfs", "Threshold", "Weighted clipping ceiling in dBFS",
        -60.0f, 0.0f, -9.0f,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_WEIGHTING_US] = g_param_spec_int(
        "weighting-us", "FM weighting", "Clip-constraint preemphasis time constant: 0, 50, or 75 us",
        0, 75, 50,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_BYPASS] = g_param_spec_boolean(
        "bypass", "Bypass", "Disable clipping; programme audio passes through bit-for-bit",
        FALSE,
        static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    properties[PROP_DETECTOR_PEAK_DBFS] = g_param_spec_float(
        "detector-peak-dbfs", "Detector peak",
        "Most recent weighted post-preemphasis estimate of the output in dBFS",
        -200.0f, 100.0f, -200.0f,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_CLIP_COUNT] = g_param_spec_uint64(
        "clip-count", "Clip count",
        "Running total of samples (summed across channels) modified, including soft knee, since the last "
        "configuration change; retained across DISCONT state resets",
        0, G_MAXUINT64, 0,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_MAX_CLIPPING_DB] = g_param_spec_float(
        "max-clipping-db", "Max clipping",
        "Running maximum weighted preemphasis threshold overshoot in dB since the last "
        "configuration change; retained across DISCONT state resets",
        0.0f, 200.0f, 0.0f,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    
    properties[PROP_BUFFER_MAX_CLIPPING_DB] = g_param_spec_float(
        "buffer-max-clipping-db", "Buffer max clipping",
        "Maximum weighted preemphasis threshold overshoot in dB in the most recently processed buffer",
        0.0f, 200.0f, 0.0f,
        static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    properties[PROP_CLIPPING_MODE] = g_param_spec_enum(
        "clipping-mode", "Clipping mode", "Hard clipping or weighted soft knee with hard ceiling",
        clipping_mode_type(), 0, static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    properties[PROP_SOFT_KNEE_DB] = g_param_spec_float(
        "soft-knee-db", "Soft knee", "Weighted knee width; applies only in soft mode; zero uses hard clipping",
        0.0f, 6.0f, 2.0f, static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    properties[PROP_HF_CORRECTION_US] = g_param_spec_int(
        "hf-correction-us", "HF correction",
        "Offset in us added to clipper detector weighting in both modes; ignored when weighting-us is 0",
        -10, 30, 0, static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_properties(gobject_class, N_PROPERTIES, properties);
}

static void gst_fmheadend_clipper_init(GstFmheadendClipper *self) {
    g_mutex_init(&self->lock);
    self->core = new (std::nothrow) fmheadend::FmClipperCore();
    if (self->core == nullptr)
        g_error("Could not allocate FM clipper DSP core");

    self->sample_rate = 0;
    self->dirty = TRUE;

    self->threshold_dbfs = -9.0f;
    self->weighting_us = 50;
    self->bypass = FALSE;
    self->clipping_mode = 0;
    self->soft_knee_db = 2.0f;
    self->hf_correction_us = 0;

    self->detector_peak_dbfs = -200.0f;

    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_qos_enabled(GST_BASE_TRANSFORM(self), TRUE);
}

static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(
        plugin, "fmheadendclipper", GST_RANK_NONE, GST_TYPE_FMHEADEND_CLIPPER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    fmheadendclipper,
    "FM-aware zero-latency hard/soft clipper for FM headend processing",
    plugin_init,
    "0.1.0",
    "MIT",
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN
)
