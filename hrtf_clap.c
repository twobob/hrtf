
#define _CRT_SECURE_NO_WARNINGS
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "hrtf_core.h"

#define PLUGIN_ID "com.example.rotating-hrtf-v2"
#define PLUGIN_NAME "Rotating HRTF v2"
#define PLUGIN_VENDOR "Example Audio"
#define PLUGIN_VERSION "2.0.0"

enum {
    PARAM_DISTANCE = 1,
    PARAM_ROTATION = 2,
    PARAM_ELEVATION = 3,
    PARAM_SPACE = 4,
    PARAM_TEST_PULSE = 5,
    PARAM_TEST_TONE = 6,
    PARAM_EAR_SCALE = 7
};

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    HrtfCore *core;
    double sample_rate;
    double distance_m;
    double rotation_phase;
    double elevation_deg;
    double space;
    double test_pulse;
    double test_tone;
    double ear_scale;
    HrtfTestGen test_gen;
    bool active;
    float *silence;            /* zeros, used when the input port is inactive */
    size_t silence_frames;
    float *test_buf;           /* scratch buffer for synthesised test pulses */
} RotatingHrtf;

static const char *features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_MONO,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL
};

static const clap_plugin_descriptor_t descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = PLUGIN_ID,
    .name = PLUGIN_NAME,
    .vendor = PLUGIN_VENDOR,
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = PLUGIN_VERSION,
    .description = "Mono-to-stereo rotating head/pinna HRTF-ready renderer.",
    .features = features
};

static RotatingHrtf *self_from_plugin(const clap_plugin_t *plugin)
{
    return (RotatingHrtf *)plugin->plugin_data;
}

static double wrap_unit(double x)
{
    x -= floor(x);
    return x < 0.0 ? x + 1.0 : x;
}

/* ------------------------------ lifecycle ------------------------------ */

static bool plugin_init(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    p->sample_rate = 48000.0;
    p->distance_m = 2.0;
    p->rotation_phase = 0.0;
    p->elevation_deg = 0.0;
    p->space = 0.15;
    p->test_pulse = 0.0;
    p->test_tone = 0.5;
    p->ear_scale = 1.0;
    hrtf_test_gen_init(&p->test_gen, p->sample_rate);
    hrtf_test_gen_set_tone(&p->test_gen, p->test_tone);
    p->active = false;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!p) return;
    if (p->core) hrtf_destroy(p->core);
    free(p->silence);
    free(p->test_buf);
    free(p);
}

static bool plugin_activate(const clap_plugin_t *plugin,
                            double sample_rate,
                            uint32_t min_frames_count,
                            uint32_t max_frames_count)
{
    (void)min_frames_count;
    RotatingHrtf *p = self_from_plugin(plugin);

    if (p->core) {
        hrtf_destroy(p->core);
        p->core = NULL;
    }

    p->sample_rate = sample_rate;
    p->core = hrtf_create(sample_rate, max_frames_count);
    if (!p->core) return false;

    free(p->silence);
    free(p->test_buf);
    p->silence_frames = max_frames_count ? max_frames_count : 1;
    p->silence = (float *)calloc(p->silence_frames, sizeof(float));
    if (!p->silence) {
        hrtf_destroy(p->core);
        p->core = NULL;
        return false;
    }
    p->test_buf = (float *)calloc(p->silence_frames, sizeof(float));
    if (!p->test_buf) {
        free(p->silence);
        p->silence = NULL;
        hrtf_destroy(p->core);
        p->core = NULL;
        return false;
    }

    hrtf_set_distance(p->core, p->distance_m);
    hrtf_set_rotation_phase(p->core, p->rotation_phase);
    hrtf_set_elevation_deg(p->core, p->elevation_deg);
    hrtf_set_space(p->core, p->space);
    hrtf_set_ear_scale(p->core, p->ear_scale);
    hrtf_test_gen_init(&p->test_gen, sample_rate);
    hrtf_test_gen_set_tone(&p->test_gen, p->test_tone);
    hrtf_reset(p->core);
    p->active = true;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    p->active = false;
    if (p->core) {
        hrtf_destroy(p->core);
        p->core = NULL;
    }
    free(p->silence);
    p->silence = NULL;
    free(p->test_buf);
    p->test_buf = NULL;
    p->silence_frames = 0;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    return p->core != NULL;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!p->core) return;
    hrtf_set_distance(p->core, p->distance_m);
    hrtf_set_rotation_phase(p->core, p->rotation_phase);
    hrtf_set_elevation_deg(p->core, p->elevation_deg);
    hrtf_set_space(p->core, p->space);
    hrtf_reset(p->core);
}

/* ------------------------------ audio ports ------------------------------ */

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    return is_input ? 1u : 1u;
}

static bool audio_ports_get(const clap_plugin_t *plugin,
                            uint32_t index,
                            bool is_input,
                            clap_audio_port_info_t *info)
{
    (void)plugin;
    if (!info || index != 0) return false;

    memset(info, 0, sizeof(*info));
    info->id = is_input ? 0u : 1u;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = is_input ? 1u : 2u;
    info->port_type = is_input ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;

    if (is_input) {
        snprintf(info->name, sizeof(info->name), "Mono Source");
    } else {
        snprintf(info->name, sizeof(info->name), "Binaural Output");
    }
    return true;
}

static const clap_plugin_audio_ports_t audio_ports_ext = {
    .count = audio_ports_count,
    .get = audio_ports_get
};

/* ------------------------------ parameters ------------------------------ */

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 7u;
}

static bool params_get_info(const clap_plugin_t *plugin,
                            uint32_t index,
                            clap_param_info_t *info)
{
    (void)plugin;
    if (!info || index >= 7u) return false;

    memset(info, 0, sizeof(*info));

    if (index == 0u) {
        info->id = PARAM_DISTANCE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Distance");
        snprintf(info->module, sizeof(info->module), "Position");
        info->min_value = 0.05;
        info->max_value = 20.0;
        info->default_value = 2.0;
    } else if (index == 1u) {
        info->id = PARAM_ROTATION;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE |
                      CLAP_PARAM_IS_PERIODIC |
                      CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Rotation Phase");
        snprintf(info->module, sizeof(info->module), "Position");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
    } else if (index == 2u) {
        info->id = PARAM_ELEVATION;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Elevation");
        snprintf(info->module, sizeof(info->module), "Position");
        info->min_value = -90.0;
        info->max_value = 90.0;
        info->default_value = 0.0;
    } else if (index == 3u) {
        info->id = PARAM_SPACE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Space");
        snprintf(info->module, sizeof(info->module), "Room");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.15;
    } else if (index == 4u) {
        info->id = PARAM_TEST_PULSE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE |
                      CLAP_PARAM_IS_STEPPED |
                      CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Test Pulse");
        snprintf(info->module, sizeof(info->module), "Generator");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
    } else if (index == 5u) {
        info->id = PARAM_TEST_TONE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Test Tone");
        snprintf(info->module, sizeof(info->module), "Generator");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.5;
    } else if (index == 6u) {
        info->id = PARAM_EAR_SCALE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Ear Scale");
        snprintf(info->module, sizeof(info->module), "Morphology");
        info->min_value = 0.70;
        info->max_value = 1.30;
        info->default_value = 1.00;
    }
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin,
                             clap_id param_id,
                             double *out_value)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!out_value) return false;

    if (param_id == PARAM_DISTANCE) {
        *out_value = p->distance_m;
        return true;
    }
    if (param_id == PARAM_ROTATION) {
        *out_value = p->rotation_phase;
        return true;
    }
    if (param_id == PARAM_ELEVATION) {
        *out_value = p->elevation_deg;
        return true;
    }
    if (param_id == PARAM_SPACE) {
        *out_value = p->space;
        return true;
    }
    if (param_id == PARAM_TEST_PULSE) {
        *out_value = p->test_pulse;
        return true;
    }
    if (param_id == PARAM_TEST_TONE) {
        *out_value = p->test_tone;
        return true;
    }
    if (param_id == PARAM_EAR_SCALE) {
        *out_value = p->ear_scale;
        return true;
    }
    return false;
}

static bool params_value_to_text(const clap_plugin_t *plugin,
                                 clap_id param_id,
                                 double value,
                                 char *out_buffer,
                                 uint32_t out_buffer_capacity)
{
    (void)plugin;
    if (!out_buffer || out_buffer_capacity == 0) return false;

    if (param_id == PARAM_DISTANCE) {
        snprintf(out_buffer, out_buffer_capacity, "%.3g m", value);
        return true;
    }
    if (param_id == PARAM_ROTATION) {
        double degrees = value * 360.0;
        if (degrees >= 359.9995) degrees = 0.0;
        snprintf(out_buffer, out_buffer_capacity, "%.1f deg", degrees);
        return true;
    }
    if (param_id == PARAM_ELEVATION) {
        snprintf(out_buffer, out_buffer_capacity, "%.1f deg", value);
        return true;
    }
    if (param_id == PARAM_SPACE) {
        snprintf(out_buffer, out_buffer_capacity, "%.0f %%", value * 100.0);
        return true;
    }
    if (param_id == PARAM_TEST_PULSE) {
        snprintf(out_buffer, out_buffer_capacity, "%s", value >= 0.5 ? "On" : "Off");
        return true;
    }
    if (param_id == PARAM_TEST_TONE) {
        snprintf(out_buffer, out_buffer_capacity, "%.0f %%", value * 100.0);
        return true;
    }
    if (param_id == PARAM_EAR_SCALE) {
        snprintf(out_buffer, out_buffer_capacity, "%.0f %%", value * 100.0);
        return true;
    }
    return false;
}

static bool params_text_to_value(const clap_plugin_t *plugin,
                                 clap_id param_id,
                                 const char *text,
                                 double *out_value)
{
    (void)plugin;
    if (!text || !out_value) return false;

    char *end = NULL;
    errno = 0;
    double v = strtod(text, &end);
    if (end == text || errno == ERANGE) return false;
    /* "nan" and "inf" parse successfully but must never reach a parameter. */
    if (!isfinite(v)) return false;

    while (*end == ' ' || *end == '\t') ++end;

    if (param_id == PARAM_DISTANCE) {
        if (*end == 'm' || *end == 'M') ++end;
        if (*end != '\0') return false;
        if (v < 0.05 || v > 20.0) return false;
        *out_value = v;
        return true;
    }

    if (param_id == PARAM_ROTATION) {
        if (*end == 'd' || *end == 'D') {
            if (end[1] == 'e' || end[1] == 'E') {
                if (end[2] == 'g' || end[2] == 'G') end += 3;
            }
        }
        if (*end != '\0') return false;

        /* Text without degrees is interpreted as phase [0,1]. */
        if (v > 1.0 || v < -1.0) {
            v = v / 360.0;
        }
        v -= floor(v);
        if (v < 0.0) v += 1.0;
        *out_value = v;
        return true;
    }

    if (param_id == PARAM_ELEVATION) {
        if (*end == 'd' || *end == 'D') {
            if (end[1] == 'e' || end[1] == 'E') {
                if (end[2] == 'g' || end[2] == 'G') end += 3;
            }
        }
        if (*end != '\0') return false;
        if (v < -90.0 || v > 90.0) return false;
        *out_value = v;
        return true;
    }

    if (param_id == PARAM_SPACE) {
        if (*end == '%') ++end;
        if (*end != '\0') return false;
        if (v > 1.0) v /= 100.0;
        if (v < 0.0 || v > 1.0) return false;
        *out_value = v;
        return true;
    }

    if (param_id == PARAM_TEST_PULSE) {
        if (_stricmp(text, "on") == 0 || strcmp(text, "1") == 0) {
            *out_value = 1.0;
            return true;
        }
        if (_stricmp(text, "off") == 0 || strcmp(text, "0") == 0) {
            *out_value = 0.0;
            return true;
        }
        *out_value = (v >= 0.5) ? 1.0 : 0.0;
        return true;
    }

    if (param_id == PARAM_TEST_TONE) {
        if (*end == '%') ++end;
        if (*end != '\0') return false;
        if (v > 1.0) v /= 100.0;
        if (v < 0.0 || v > 1.0) return false;
        *out_value = v;
        return true;
    }

    if (param_id == PARAM_EAR_SCALE) {
        if (*end == '%') ++end;
        if (*end != '\0') return false;
        if (v > 10.0) v /= 100.0;
        if (v < 0.70 || v > 1.30) return false;
        *out_value = v;
        return true;
    }

    return false;
}

static void params_apply_value(RotatingHrtf *p, clap_id id, double value)
{
    /* Never store a non-finite parameter: it would be reported back to the
       host through get_value and written into any saved state. */
    if (!isfinite(value)) return;

    if (id == PARAM_DISTANCE) {
        if (value < 0.05) value = 0.05;
        if (value > 20.0) value = 20.0;
        p->distance_m = value;
        if (p->core) hrtf_set_distance(p->core, value);
    } else if (id == PARAM_ROTATION) {
        value -= floor(value);
        if (value < 0.0) value += 1.0;
        p->rotation_phase = value;
        if (p->core) hrtf_set_rotation_phase(p->core, value);
    } else if (id == PARAM_ELEVATION) {
        if (value < -90.0) value = -90.0;
        if (value > 90.0) value = 90.0;
        p->elevation_deg = value;
        if (p->core) hrtf_set_elevation_deg(p->core, value);
    } else if (id == PARAM_SPACE) {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        p->space = value;
        if (p->core) hrtf_set_space(p->core, value);
    } else if (id == PARAM_TEST_PULSE) {
        p->test_pulse = (value >= 0.5) ? 1.0 : 0.0;
    } else if (id == PARAM_TEST_TONE) {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        p->test_tone = value;
        hrtf_test_gen_set_tone(&p->test_gen, value);
    } else if (id == PARAM_EAR_SCALE) {
        if (value < 0.70) value = 0.70;
        if (value > 1.30) value = 1.30;
        p->ear_scale = value;
        if (p->core) hrtf_set_ear_scale(p->core, value);
    }
}

static void params_flush(const clap_plugin_t *plugin,
                         const clap_input_events_t *in,
                         const clap_output_events_t *out)
{
    (void)out;
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!in) return;

    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t *h = in->get(in, i);
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            h->type != CLAP_EVENT_PARAM_VALUE)
            continue;

        const clap_event_param_value_t *e =
            (const clap_event_param_value_t *)h;
        params_apply_value(p, e->param_id, e->value);
    }
}

static const clap_plugin_params_t params_ext = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush
};

/* ------------------------------ state ------------------------------ */

static const char kStateMagic[4] = { 'H', 'R', 'T', 'F' };
static const uint32_t kStateVersion = 4;

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!stream || !stream->write) return false;

    double values[7];
    values[0] = p->distance_m;
    values[1] = p->rotation_phase;
    values[2] = p->elevation_deg;
    values[3] = p->space;
    values[4] = p->test_pulse;
    values[5] = p->test_tone;
    values[6] = p->ear_scale;

    if (stream->write(stream, kStateMagic, sizeof(kStateMagic)) != (int64_t)sizeof(kStateMagic))
        return false;
    if (stream->write(stream, &kStateVersion, sizeof(kStateVersion)) != (int64_t)sizeof(kStateVersion))
        return false;
    if (stream->write(stream, values, sizeof(values)) != (int64_t)sizeof(values))
        return false;

    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!stream || !stream->read) return false;

    char magic[4];
    uint32_t version = 0;

    if (stream->read(stream, magic, sizeof(magic)) != (int64_t)sizeof(magic)) return false;
    if (memcmp(magic, kStateMagic, sizeof(magic)) != 0) return false;
    if (stream->read(stream, &version, sizeof(version)) != (int64_t)sizeof(version)) return false;
    if (version != 1 && version != 2 && version != 3 && version != kStateVersion) return false;

    if (version == 1) {
        double values[2] = { 0.0, 0.0 };
        if (stream->read(stream, values, sizeof(values)) != (int64_t)sizeof(values)) return false;
        if (isfinite(values[0]) && values[0] >= 0.05 && values[0] <= 20.0) {
            p->distance_m = values[0];
            if (p->core) hrtf_set_distance(p->core, values[0]);
        }
        if (isfinite(values[1])) {
            double phase = wrap_unit(values[1]);
            p->rotation_phase = phase;
            if (p->core) hrtf_set_rotation_phase(p->core, phase);
        }
        p->elevation_deg = 0.0;
        p->space = 0.15;
        p->test_pulse = 0.0;
        p->test_tone = 0.5;
        p->ear_scale = 1.0;
        if (p->core) {
            hrtf_set_elevation_deg(p->core, 0.0);
            hrtf_set_space(p->core, 0.15);
            hrtf_set_ear_scale(p->core, 1.0);
        }
        hrtf_test_gen_set_tone(&p->test_gen, 0.5);
    } else if (version == 2) {
        double values[4] = { 0.0, 0.0, 0.0, 0.0 };
        if (stream->read(stream, values, sizeof(values)) != (int64_t)sizeof(values)) return false;
        if (isfinite(values[0]) && values[0] >= 0.05 && values[0] <= 20.0) {
            p->distance_m = values[0];
            if (p->core) hrtf_set_distance(p->core, values[0]);
        }
        if (isfinite(values[1])) {
            double phase = wrap_unit(values[1]);
            p->rotation_phase = phase;
            if (p->core) hrtf_set_rotation_phase(p->core, phase);
        }
        if (isfinite(values[2]) && values[2] >= -90.0 && values[2] <= 90.0) {
            p->elevation_deg = values[2];
            if (p->core) hrtf_set_elevation_deg(p->core, values[2]);
        }
        if (isfinite(values[3]) && values[3] >= 0.0 && values[3] <= 1.0) {
            p->space = values[3];
            if (p->core) hrtf_set_space(p->core, values[3]);
        }
        p->test_pulse = 0.0;
        p->test_tone = 0.5;
        p->ear_scale = 1.0;
        if (p->core) {
            hrtf_set_ear_scale(p->core, 1.0);
        }
        hrtf_test_gen_set_tone(&p->test_gen, 0.5);
    } else if (version == 3) {
        double values[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
        if (stream->read(stream, values, sizeof(values)) != (int64_t)sizeof(values)) return false;
        if (isfinite(values[0]) && values[0] >= 0.05 && values[0] <= 20.0) {
            p->distance_m = values[0];
            if (p->core) hrtf_set_distance(p->core, values[0]);
        }
        if (isfinite(values[1])) {
            double phase = wrap_unit(values[1]);
            p->rotation_phase = phase;
            if (p->core) hrtf_set_rotation_phase(p->core, phase);
        }
        if (isfinite(values[2]) && values[2] >= -90.0 && values[2] <= 90.0) {
            p->elevation_deg = values[2];
            if (p->core) hrtf_set_elevation_deg(p->core, values[2]);
        }
        if (isfinite(values[3]) && values[3] >= 0.0 && values[3] <= 1.0) {
            p->space = values[3];
            if (p->core) hrtf_set_space(p->core, values[3]);
        }
        if (isfinite(values[4]) && values[4] >= 0.0 && values[4] <= 1.0) {
            p->test_pulse = (values[4] >= 0.5) ? 1.0 : 0.0;
        }
        p->test_tone = 0.5;
        p->ear_scale = 1.0;
        if (p->core) {
            hrtf_set_ear_scale(p->core, 1.0);
        }
        hrtf_test_gen_set_tone(&p->test_gen, 0.5);
    } else if (version == 4) {
        double values[7] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 1.0 };
        if (stream->read(stream, values, sizeof(values)) != (int64_t)sizeof(values)) return false;
        if (isfinite(values[0]) && values[0] >= 0.05 && values[0] <= 20.0) {
            p->distance_m = values[0];
            if (p->core) hrtf_set_distance(p->core, values[0]);
        }
        if (isfinite(values[1])) {
            double phase = wrap_unit(values[1]);
            p->rotation_phase = phase;
            if (p->core) hrtf_set_rotation_phase(p->core, phase);
        }
        if (isfinite(values[2]) && values[2] >= -90.0 && values[2] <= 90.0) {
            p->elevation_deg = values[2];
            if (p->core) hrtf_set_elevation_deg(p->core, values[2]);
        }
        if (isfinite(values[3]) && values[3] >= 0.0 && values[3] <= 1.0) {
            p->space = values[3];
            if (p->core) hrtf_set_space(p->core, values[3]);
        }
        if (isfinite(values[4]) && values[4] >= 0.0 && values[4] <= 1.0) {
            p->test_pulse = (values[4] >= 0.5) ? 1.0 : 0.0;
        }
        if (isfinite(values[5]) && values[5] >= 0.0 && values[5] <= 1.0) {
            p->test_tone = values[5];
            hrtf_test_gen_set_tone(&p->test_gen, values[5]);
        }
        if (isfinite(values[6]) && values[6] >= 0.70 && values[6] <= 1.30) {
            p->ear_scale = values[6];
            if (p->core) hrtf_set_ear_scale(p->core, values[6]);
        }
    }

    return true;
}

static const clap_plugin_state_t state_ext = {
    .save = state_save,
    .load = state_load
};

/* ------------------------------ processing ------------------------------ */

static void apply_event(RotatingHrtf *p, const clap_event_header_t *h)
{
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID ||
        h->type != CLAP_EVENT_PARAM_VALUE)
        return;

    const clap_event_param_value_t *e =
        (const clap_event_param_value_t *)h;
    params_apply_value(p, e->param_id, e->value);
}

static clap_process_status plugin_process(const clap_plugin_t *plugin,
                                          const clap_process_t *process)
{
    RotatingHrtf *p = self_from_plugin(plugin);

    if (!p->core || !process)
        return CLAP_PROCESS_ERROR;

    /* A port the host has deactivated is expressed as a null data32 array.
       Nothing to render into is not an error, and neither is silence in. */
    const clap_audio_buffer_t *in =
        process->audio_inputs_count > 0 ? &process->audio_inputs[0] : NULL;
    clap_audio_buffer_t *out =
        process->audio_outputs_count > 0 ? &process->audio_outputs[0] : NULL;

    if (!out || !out->data32 || out->channel_count < 2 ||
        !out->data32[0] || !out->data32[1])
        return CLAP_PROCESS_CONTINUE;

    const bool input_active =
        in && in->data32 && in->channel_count >= 1 && in->data32[0];

    if (!input_active && process->frames_count > p->silence_frames)
        return CLAP_PROCESS_ERROR; /* host broke the max_frames_count contract */

    const float *src = input_active ? in->data32[0] : p->silence;
    float *dst_l = out->data32[0];
    float *dst_r = out->data32[1];

    float *dst[2] = { dst_l, dst_r };

    uint32_t frame_count = process->frames_count;
    uint32_t cursor = 0;
    uint32_t event_index = 0;
    uint32_t event_count = process->in_events ? process->in_events->size(process->in_events) : 0;

    while (cursor < frame_count) {
        uint32_t next = frame_count;

        if (process->in_events && event_index < event_count) {
            const clap_event_header_t *h =
                process->in_events->get(process->in_events, event_index);

            if (h) {
                uint32_t t = h->time;
                if (t < cursor) t = cursor;
                if (t < next) next = t;
            } else {
                ++event_index;
                continue;
            }
        }

        if (next > cursor) {
            const uint32_t slice = next - cursor;
            const float *segment_src = NULL;

            if (p->test_pulse >= 0.5) {
                double bpm = 120.0;
                double beat_pos = 0.0;
                int is_playing = 0;
                if (process->transport) {
                    if ((process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && process->transport->tempo > 1.0)
                        bpm = process->transport->tempo;
                    if (process->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
                        beat_pos = (double)process->transport->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
                    if (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING)
                        is_playing = 1;
                }
                double slice_beat = beat_pos;
                if (is_playing && p->sample_rate > 1000.0) {
                    slice_beat += (double)cursor * (bpm / (60.0 * p->sample_rate));
                }
                hrtf_test_gen_process(&p->test_gen, p->test_buf, slice, bpm, slice_beat, is_playing);
                segment_src = p->test_buf;
            } else {
                segment_src = src + cursor;
            }

            hrtf_process(p->core, segment_src, dst, slice);

            /* hrtf_process writes from the start of dst, so offset the
               output pointers for the next segment. */
            dst[0] += slice;
            dst[1] += slice;
            cursor = next;
        }

        while (process->in_events && event_index < event_count) {
            const clap_event_header_t *h =
                process->in_events->get(process->in_events, event_index);
            if (!h) {
                ++event_index;
                continue;
            }
            if (h->time > cursor) break;
            apply_event(p, h);
            ++event_index;
        }

        if (next == frame_count) break;

        /* If an event occurs at the current cursor, the loop above applies it.
           If the event list contains pathological duplicate/old timestamps,
           the event index still advances, preventing an infinite loop. */
    }

    /* No-event path and any remaining frames. */
    if (cursor < frame_count) {
        const uint32_t slice = frame_count - cursor;
        const float *segment_src = NULL;

        if (p->test_pulse >= 0.5) {
            double bpm = 120.0;
            double beat_pos = 0.0;
            int is_playing = 0;
            if (process->transport) {
                if ((process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && process->transport->tempo > 1.0)
                    bpm = process->transport->tempo;
                if (process->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
                    beat_pos = (double)process->transport->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
                if (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING)
                    is_playing = 1;
            }
            double slice_beat = beat_pos;
            if (is_playing && p->sample_rate > 1000.0) {
                slice_beat += (double)cursor * (bpm / (60.0 * p->sample_rate));
            }
            hrtf_test_gen_process(&p->test_gen, p->test_buf, slice, bpm, slice_beat, is_playing);
            segment_src = p->test_buf;
        } else {
            segment_src = src + cursor;
        }

        hrtf_process(p->core, segment_src, dst, slice);
    }

    return CLAP_PROCESS_CONTINUE;
}

/* ------------------------------ extension dispatch ------------------------------ */

static const void *plugin_get_extension(const clap_plugin_t *plugin,
                                        const char *id)
{
    (void)plugin;

    if (!id) return NULL;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports_ext;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &params_ext;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &state_ext;

    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

/* ------------------------------ factory ------------------------------ */

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory,
                                           const clap_host_t *host,
                                           const char *plugin_id)
{
    (void)factory;

    if (!host || !plugin_id || strcmp(plugin_id, PLUGIN_ID) != 0)
        return NULL;

    RotatingHrtf *p = (RotatingHrtf *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->host = host;
    p->distance_m = 2.0;
    p->rotation_phase = 0.0;
    p->elevation_deg = 0.0;
    p->space = 0.15;

    p->plugin.desc = &descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = plugin_init;
    p->plugin.destroy = plugin_destroy;
    p->plugin.activate = plugin_activate;
    p->plugin.deactivate = plugin_deactivate;
    p->plugin.start_processing = plugin_start_processing;
    p->plugin.stop_processing = plugin_stop_processing;
    p->plugin.reset = plugin_reset;
    p->plugin.process = plugin_process;
    p->plugin.get_extension = plugin_get_extension;
    p->plugin.on_main_thread = plugin_on_main_thread;

    return &p->plugin;
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return 1u;
}

static const clap_plugin_descriptor_t *
factory_get_plugin_descriptor(const clap_plugin_factory_t *factory,
                              uint32_t index)
{
    (void)factory;
    return index == 0u ? &descriptor : NULL;
}

static const clap_plugin_factory_t factory = {
    .get_plugin_count = factory_get_plugin_count,
    .get_plugin_descriptor = factory_get_plugin_descriptor,
    .create_plugin = factory_create
};

/* ------------------------------ CLAP entry ------------------------------ */

static bool entry_init(const char *plugin_path)
{
    (void)plugin_path;
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    if (!factory_id) return NULL;
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory
};
