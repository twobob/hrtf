
#define _CRT_SECURE_NO_WARNINGS
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/params.h>
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
    PARAM_ROTATION = 2
};

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    HrtfCore *core;
    double sample_rate;
    double distance_m;
    double rotation_phase;
    bool active;
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

/* ------------------------------ lifecycle ------------------------------ */

static bool plugin_init(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    p->sample_rate = 48000.0;
    p->distance_m = 2.0;
    p->rotation_phase = 0.0;
    p->active = false;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    RotatingHrtf *p = self_from_plugin(plugin);
    if (!p) return;
    if (p->core) hrtf_destroy(p->core);
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

    hrtf_set_distance(p->core, p->distance_m);
    hrtf_set_rotation_phase(p->core, p->rotation_phase);
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
    return 2u;
}

static bool params_get_info(const clap_plugin_t *plugin,
                            uint32_t index,
                            clap_param_info_t *info)
{
    (void)plugin;
    if (!info || index >= 2u) return false;

    memset(info, 0, sizeof(*info));

    if (index == 0u) {
        info->id = PARAM_DISTANCE;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Distance");
        snprintf(info->module, sizeof(info->module), "Position");
        info->min_value = 0.05;
        info->max_value = 20.0;
        info->default_value = 2.0;
    } else {
        info->id = PARAM_ROTATION;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE |
                      CLAP_PARAM_IS_PERIODIC |
                      CLAP_PARAM_REQUIRES_PROCESS;
        snprintf(info->name, sizeof(info->name), "Rotation Phase");
        snprintf(info->module, sizeof(info->module), "Position");
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
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

    return false;
}

static void params_apply_value(RotatingHrtf *p, clap_id id, double value)
{
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

    if (!p->core || !process ||
        process->audio_inputs_count < 1 ||
        process->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;

    const clap_audio_buffer_t *in = &process->audio_inputs[0];
    clap_audio_buffer_t *out = &process->audio_outputs[0];

    if (!in->data32 || !out->data32 ||
        !in->data32[0] || !out->data32[0] || !out->data32[1])
        return CLAP_PROCESS_ERROR;

    const float *src = in->data32[0];
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
            hrtf_process(p->core, src + cursor, dst, next - cursor);

            /* hrtf_process writes from the start of dst, so offset the
               output pointers for the next segment. */
            dst[0] += next - cursor;
            dst[1] += next - cursor;
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
        hrtf_process(p->core, src + cursor, dst, frame_count - cursor);
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
