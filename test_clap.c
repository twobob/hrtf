#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <clap/clap.h>
#include "hrtf_core.h" /* distance taper helpers and HRTF_STATE_VERSION only */

/* The CLAP Distance parameter is a 0..1 position on a logarithmic taper. */
static double pos_m(double metres)
{
    return hrtf_position_from_distance(metres);
}

/* Minimal input-event list carrying a single event. */
typedef struct {
    const clap_event_header_t *event;
} SingleEvent;

static uint32_t single_events_size(const clap_input_events_t *list)
{
    const SingleEvent *s = (const SingleEvent *)list->ctx;
    return s->event ? 1u : 0u;
}

static const clap_event_header_t *single_events_get(const clap_input_events_t *list,
                                                    uint32_t index)
{
    const SingleEvent *s = (const SingleEvent *)list->ctx;
    return index == 0u ? s->event : NULL;
}

/* Minimal in-memory clap_ostream / clap_istream pair, for state round-trips. */
typedef struct {
    uint8_t data[256];
    size_t  size;
    size_t  pos;
} MemStream;

static int64_t mem_write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
    MemStream *m = (MemStream *)stream->ctx;
    if (m->size + (size_t)size > sizeof(m->data)) return -1;
    memcpy(m->data + m->size, buffer, (size_t)size);
    m->size += (size_t)size;
    return (int64_t)size;
}

static int64_t mem_read(const clap_istream_t *stream, void *buffer, uint64_t size)
{
    MemStream *m = (MemStream *)stream->ctx;
    if (m->pos + (size_t)size > m->size) return -1;
    memcpy(buffer, m->data + m->pos, (size_t)size);
    m->pos += (size_t)size;
    return (int64_t)size;
}

/* Input-event list backed by an array of param-value events. */
typedef struct {
    const clap_event_param_value_t *events;
    uint32_t count;
} EventArray;

static uint32_t array_events_size(const clap_input_events_t *list)
{
    return ((const EventArray *)list->ctx)->count;
}

static const clap_event_header_t *array_events_get(const clap_input_events_t *list, uint32_t index)
{
    const EventArray *a = (const EventArray *)list->ctx;
    return index < a->count ? &a->events[index].header : NULL;
}

static clap_event_param_value_t make_param_event(uint32_t time, clap_id id, double value)
{
    clap_event_param_value_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.param_id = id;
    ev.value = value;
    return ev;
}

/* Pink noise (Kellet filter over xorshift white noise) for level checks. */
static void make_pink_noise(float *out, size_t n, uint32_t seed, float gain)
{
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    uint32_t s = seed;
    for (size_t i = 0; i < n; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const double w = (double)(int32_t)s / 2147483648.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        out[i] = (float)(p * gain);
    }
}

/* Streams `noise` through the plugin in 256-frame blocks (process already
   points at in/out_l/out_r) and returns each ear's level in dB relative to
   the input over the blocks after `settle`. */
static void measure_levels(const clap_plugin_t *plugin, const clap_process_t *process,
                           float *in, const float *out_l, const float *out_r,
                           const float *noise, int settle, int blocks,
                           double *level_l, double *level_r)
{
    double pin = 0.0, pl = 0.0, pr = 0.0;
    for (int b = 0; b < settle + blocks; ++b) {
        memcpy(in, noise + (size_t)b * 256, 256 * sizeof(float));
        plugin->process(plugin, process);
        if (b < settle) continue;
        for (int i = 0; i < 256; ++i) {
            pin += (double)in[i] * in[i];
            pl += (double)out_l[i] * out_l[i];
            pr += (double)out_r[i] * out_r[i];
        }
    }
    *level_l = 10.0 * log10(pl / pin);
    *level_r = 10.0 * log10(pr / pin);
}

static void flush_param(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                        clap_id id, double value)
{
    clap_event_param_value_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.param_id = id;
    ev.value = value;

    SingleEvent se = { &ev.header };
    clap_input_events_t list = { &se, single_events_size, single_events_get };
    params->flush(plugin, &list, NULL);
}

#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];
    uint32_t riff_size;
    char     wave_id[4];
} RiffHeader;

typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
} ChunkHeader;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

static int load_test_wav(const char *filename, float *buffer, size_t max_samples, size_t *out_samples)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;

    RiffHeader riff;
    if (fread(&riff, sizeof(riff), 1, f) != 1 ||
        memcmp(riff.riff_id, "RIFF", 4) != 0 ||
        memcmp(riff.wave_id, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }

    FmtChunk fmt;
    memset(&fmt, 0, sizeof(fmt));
    int has_fmt = 0;
    size_t samples_read = 0;

    while (!feof(f)) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;

        if (memcmp(ch.chunk_id, "fmt ", 4) == 0) {
            size_t to_read = ch.chunk_size < sizeof(fmt) ? ch.chunk_size : sizeof(fmt);
            if (fread(&fmt, to_read, 1, f) != 1) { fclose(f); return 0; }
            if (ch.chunk_size > to_read) fseek(f, (long)(ch.chunk_size - to_read), SEEK_CUR);
            has_fmt = 1;
        } else if (memcmp(ch.chunk_id, "data", 4) == 0) {
            if (!has_fmt || fmt.num_channels != 1 || (fmt.bits_per_sample != 24 && fmt.bits_per_sample != 16)) {
                fclose(f);
                return 0;
            }

            size_t bytes_per_sample = fmt.bits_per_sample / 8;
            size_t total_samples = ch.chunk_size / bytes_per_sample;
            size_t count = total_samples < max_samples ? total_samples : max_samples;

            for (size_t i = 0; i < count; ++i) {
                if (bytes_per_sample == 3) {
                    uint8_t b[3];
                    if (fread(b, 1, 3, f) != 3) break;
                    int32_t val = (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16));
                    if (val & 0x800000) val |= 0xFF000000;
                    buffer[i] = (float)val / 8388608.0f;
                } else if (bytes_per_sample == 2) {
                    int16_t val;
                    if (fread(&val, 2, 1, f) != 1) break;
                    buffer[i] = (float)val / 32768.0f;
                }
                samples_read++;
            }
            break;
        } else {
            fseek(f, (long)ch.chunk_size, SEEK_CUR);
        }
    }

    fclose(f);
    if (out_samples) *out_samples = samples_read;
    return samples_read > 0 ? 1 : 0;
}

static int g_host_rescan_called = 0;
static clap_param_rescan_flags g_host_last_rescan_flags = 0;

static void CLAP_ABI dummy_rescan(const clap_host_t *host, clap_param_rescan_flags flags) {
    (void)host;
    g_host_rescan_called++;
    g_host_last_rescan_flags = flags;
}

static void CLAP_ABI dummy_clear(const clap_host_t *host, clap_id param_id, clap_param_clear_flags flags) {
    (void)host; (void)param_id; (void)flags;
}

static void CLAP_ABI dummy_request_flush(const clap_host_t *host) {
    (void)host;
}

static const clap_host_params_t dummy_host_params = {
    .rescan = dummy_rescan,
    .clear = dummy_clear,
    .request_flush = dummy_request_flush
};

static const void * CLAP_ABI dummy_get_ext(const clap_host_t *host, const char *extension_id) {
    (void)host;
    if (extension_id && strcmp(extension_id, CLAP_EXT_PARAMS) == 0) {
        return &dummy_host_params;
    }
    return NULL;
}

int main(void) {
    /* Fail the build without popping up a crash dialog if this harness
       ever faults: the exit code is what the build script reads. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    int failures = 0;

    printf("Loading RotatingHRTF_v2.clap...\n");
    HMODULE lib = LoadLibraryA("RotatingHRTF_v2.clap");
    if (!lib) {
        printf("Failed to load RotatingHRTF_v2.clap. Error: %lu\n", GetLastError());
        return 1;
    }

    const clap_plugin_entry_t *entry = (const clap_plugin_entry_t *)GetProcAddress(lib, "clap_entry");
    if (!entry) {
        printf("Failed to find clap_entry symbol\n");
        FreeLibrary(lib);
        return 1;
    }

    if (!entry->init(".")) {
        printf("clap_entry->init() failed\n");
        FreeLibrary(lib);
        return 1;
    }

    const clap_plugin_factory_t *factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory) {
        printf("Failed to get CLAP factory\n");
        entry->deinit();
        FreeLibrary(lib);
        return 1;
    }

    uint32_t count = factory->get_plugin_count(factory);
    printf("Plugin count: %u\n", count);
    if (count == 0) {
        printf("No plugins in factory!\n");
        entry->deinit();
        FreeLibrary(lib);
        return 1;
    }

    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    printf("Plugin: %s (id: %s, vendor: %s, version: %s)\n",
           desc->name, desc->id, desc->vendor, desc->version);

    clap_host_t dummy_host = {
        .clap_version = CLAP_VERSION_INIT,
        .host_data = NULL,
        .name = "TestHost",
        .vendor = "Tester",
        .url = "",
        .version = "1.0",
        .get_extension = dummy_get_ext,
        .request_restart = NULL,
        .request_process = NULL,
        .request_callback = NULL
    };

    const clap_plugin_t *plugin = factory->create_plugin(factory, &dummy_host, desc->id);
    if (!plugin) {
        printf("Failed to create plugin instance\n");
        return 1;
    }

    if (!plugin->init(plugin)) {
        printf("Plugin init failed\n");
        return 1;
    }

    if (!plugin->activate(plugin, 48000.0, 32, 256)) {
        printf("Plugin activate failed\n");
        return 1;
    }

    // Dogfood generated test pulse audio file: load pulsed_pink_noise_48k.wav
    const uint32_t N = 256;
    float wav_buf[4096] = {0};
    size_t wav_count = 0;
    int loaded = load_test_wav("pulsed_pink_noise_48k.wav", wav_buf, 4096, &wav_count);
    if (!loaded || wav_count < 256) {
        printf("ERROR: Failed to load generated test pulse WAV file 'pulsed_pink_noise_48k.wav'\n");
        ++failures;
    } else {
        float max_peak = 0.0f;
        for (size_t i = 0; i < wav_count; ++i) {
            if (fabsf(wav_buf[i]) > max_peak) max_peak = fabsf(wav_buf[i]);
        }
        if (max_peak > 0.999f || max_peak < 0.50f) {
            printf("ERROR: Loaded WAV peak (%f) is out of expected calibrated bounds [0.50, 0.999]\n", max_peak);
            ++failures;
        } else {
            printf("SUCCESS: Loaded generated test pulse WAV file 'pulsed_pink_noise_48k.wav' (%zu samples, peak=%f, within calibrated band [0.50, 0.999]).\n",
                   wav_count, max_peak);
        }
    }

    float in_buf[256];
    float out_l[256] = {0};
    float out_r[256] = {0};
    for (uint32_t i = 0; i < N; ++i) {
        in_buf[i] = (loaded && (size_t)(i + 100) < wav_count) ? wav_buf[i + 100] : sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
    }

    float *in_ptrs[1] = { in_buf };
    float *out_ptrs[2] = { out_l, out_r };

    clap_audio_buffer_t in_audio = {
        .data32 = in_ptrs,
        .channel_count = 1,
        .latency = 0,
        .constant_mask = 0
    };

    clap_audio_buffer_t out_audio = {
        .data32 = out_ptrs,
        .channel_count = 2,
        .latency = 0,
        .constant_mask = 0
    };

    clap_process_t process = {
        .steady_time = 0,
        .frames_count = N,
        .transport = NULL,
        .audio_inputs = &in_audio,
        .audio_outputs = &out_audio,
        .audio_inputs_count = 1,
        .audio_outputs_count = 1,
        .in_events = NULL,
        .out_events = NULL
    };

    clap_process_status status = plugin->process(plugin, &process);
    printf("Process status: %d (CLAP_PROCESS_CONTINUE = %d)\n", status, CLAP_PROCESS_CONTINUE);
    if (status != CLAP_PROCESS_CONTINUE) {
        printf("ERROR: process() returned %d, expected CLAP_PROCESS_CONTINUE\n", status);
        ++failures;
    }

    float sum_l = 0.0f, sum_r = 0.0f;
    for (uint32_t i = 0; i < N; ++i) {
        sum_l += fabsf(out_l[i]);
        sum_r += fabsf(out_r[i]);
    }
    printf("Output energy: Left = %f, Right = %f\n", sum_l, sum_r);

    if (sum_l > 0.01f && sum_r > 0.01f) {
        printf("SUCCESS: Plugin generated valid binaural audio!\n");
    } else {
        printf("ERROR: Audio output was unexpectedly silent or zero!\n");
        ++failures;
    }

    /* Hostile parameter input must not poison the DSP state: a NaN reaching
       the phase smoother silences the output for the rest of the session. */
    const clap_plugin_params_t *params =
        (const clap_plugin_params_t *)plugin->get_extension(plugin, CLAP_EXT_PARAMS);
    if (params && params->flush && params->get_value) {
        if (params->count(plugin) != 8u) {
            printf("ERROR: params count is %u, expected 8\n", params->count(plugin));
            ++failures;
        } else {
            printf("SUCCESS: params count is 8 as expected.\n");
        }

        /* Test parameter string conversions (value_to_text & text_to_value) across all 8 parameters */
        if (params->value_to_text && params->text_to_value && params->get_info) {
            int string_conv_ok = 1;
            for (uint32_t p_idx = 0; p_idx < 8u; ++p_idx) {
                clap_param_info_t info;
                if (!params->get_info(plugin, p_idx, &info)) {
                    printf("ERROR: get_info failed for parameter index %u\n", p_idx);
                    string_conv_ok = 0;
                    break;
                }
                char text_buf[64] = {0};
                if (!params->value_to_text(plugin, info.id, info.default_value, text_buf, sizeof(text_buf))) {
                    printf("ERROR: value_to_text failed for parameter '%s' (id=%u)\n", info.name, info.id);
                    string_conv_ok = 0;
                    break;
                }
                double parsed_val = 0.0;
                if (!params->text_to_value(plugin, info.id, text_buf, &parsed_val)) {
                    printf("ERROR: text_to_value failed to parse '%s' for parameter '%s'\n", text_buf, info.name);
                    string_conv_ok = 0;
                    break;
                }
                double diff = fabs(parsed_val - info.default_value);
                if (diff > 0.05) {
                    printf("ERROR: string conversion round-trip mismatch for '%s': expected %f, got %f (text='%s')\n",
                           info.name, info.default_value, parsed_val, text_buf);
                    string_conv_ok = 0;
                    break;
                }
            }
            if (string_conv_ok) {
                printf("SUCCESS: Parameter string conversions (value_to_text and text_to_value) round-tripped with exact values for all 8 parameters.\n");
            } else {
                printf("ERROR: Parameter string conversion validation failed.\n");
                ++failures;
            }
        } else {
            printf("ERROR: value_to_text or text_to_value extension function missing.\n");
            ++failures;
        }

        /* Test latency extension */
        const clap_plugin_latency_t *lat = (const clap_plugin_latency_t *)plugin->get_extension(plugin, CLAP_EXT_LATENCY);
        if (lat && lat->get) {
            uint32_t l = lat->get(plugin);
            if (l == 2u) {
                printf("SUCCESS: CLAP latency correctly declared as 2 samples.\n");
            } else {
                printf("ERROR: CLAP latency declared as %u samples, expected 2.\n", l);
                ++failures;
            }
        } else {
            printf("ERROR: CLAP latency extension missing.\n");
            ++failures;
        }

        clap_event_param_value_t nan_event;
        memset(&nan_event, 0, sizeof(nan_event));
        nan_event.header.size = sizeof(nan_event);
        nan_event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        nan_event.header.type = CLAP_EVENT_PARAM_VALUE;
        nan_event.header.time = 0;
        nan_event.param_id = 2; /* PARAM_ROTATION in hrtf_clap.c */
        nan_event.value = (double)NAN;

        SingleEvent se = { &nan_event.header };
        clap_input_events_t in_list = { &se, single_events_size, single_events_get };
        params->flush(plugin, &in_list, NULL);

        double v = 0.0;
        if (params->get_value(plugin, 2, &v) && !isfinite(v)) {
            printf("ERROR: a NaN parameter value was accepted\n");
            ++failures;
        }

        for (uint32_t i = 0; i < N; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        plugin->process(plugin, &process);
        for (uint32_t i = 0; i < N; ++i) {
            if (!isfinite(out_l[i]) || !isfinite(out_r[i])) {
                printf("ERROR: output is not finite after a NaN parameter value\n");
                ++failures;
                break;
            }
        }
    } else {
        printf("ERROR: the params extension is missing\n");
        ++failures;
    }

    /* Inactive ports: hosts express them as null buffer arrays. That is a
       legal state, not a plugin error, and silence in must still render. */
    {
        clap_audio_buffer_t saved_in = in_audio;
        clap_audio_buffer_t saved_out = out_audio;

        out_audio.data32 = NULL;
        status = plugin->process(plugin, &process);
        if (status != CLAP_PROCESS_CONTINUE) {
            printf("ERROR: process() with an inactive output port returned %d\n", status);
            ++failures;
        }

        out_audio = saved_out;
        out_audio.channel_count = 1;
        status = plugin->process(plugin, &process);
        if (status != CLAP_PROCESS_CONTINUE) {
            printf("ERROR: process() with a mono output port returned %d\n", status);
            ++failures;
        }

        out_audio = saved_out;
        in_audio.data32 = NULL;
        for (uint32_t i = 0; i < N; ++i) { out_l[i] = 0.0f; out_r[i] = 0.0f; }
        status = plugin->process(plugin, &process);
        if (status != CLAP_PROCESS_CONTINUE) {
            printf("ERROR: process() with an inactive input port returned %d\n", status);
            ++failures;
        }
        for (uint32_t i = 0; i < N; ++i) {
            if (!isfinite(out_l[i]) || !isfinite(out_r[i])) {
                printf("ERROR: non-finite output for an inactive input port\n");
                ++failures;
                break;
            }
        }

        in_audio = saved_in;
        out_audio = saved_out;
    }

    /* State round-trip: a host saves and restores the plugin through the
       state extension, which this plugin previously did not implement at
       all, so CLAP sessions lost their settings on reload. */
    {
        const clap_plugin_state_t *state =
            (const clap_plugin_state_t *)plugin->get_extension(plugin, CLAP_EXT_STATE);
        if (!state || !state->save || !state->load) {
            printf("ERROR: the state extension is missing\n");
            ++failures;
        } else {
            MemStream mem;
            memset(&mem, 0, sizeof(mem));
            clap_ostream_t os = { &mem, mem_write };
            clap_istream_t is = { &mem, mem_read };

            flush_param(plugin, params, 2, 0.75);   /* rotation phase */
            flush_param(plugin, params, 1, pos_m(8.0));    /* distance, metres */
            flush_param(plugin, params, 3, 30.0);   /* elevation, degrees */
            flush_param(plugin, params, 4, 0.40);   /* space, 0..1 */
            flush_param(plugin, params, 5, 1.0);    /* test pulse, on */
            flush_param(plugin, params, 6, 0.85);   /* test tone, 0..1 */
            flush_param(plugin, params, 7, 1.15);   /* ear scale, 0.70..1.30 */
            flush_param(plugin, params, 8, 0.0);    /* reflections, off */

            if (!state->save(plugin, &os)) {
                printf("ERROR: state save failed\n");
                ++failures;
            }

            flush_param(plugin, params, 2, 0.25);
            flush_param(plugin, params, 1, pos_m(1.0));
            flush_param(plugin, params, 3, 0.0);
            flush_param(plugin, params, 4, 0.0);
            flush_param(plugin, params, 5, 0.0);
            flush_param(plugin, params, 6, 0.5);
            flush_param(plugin, params, 7, 1.0);
            flush_param(plugin, params, 8, 1.0);

            g_host_rescan_called = 0;
            mem.pos = 0;
            if (!state->load(plugin, &is)) {
                printf("ERROR: state load failed\n");
                ++failures;
            }
            if (g_host_rescan_called < 1 || !(g_host_last_rescan_flags & CLAP_PARAM_RESCAN_VALUES)) {
                printf("ERROR: state->load() did not notify host via clap_host_params.rescan(CLAP_PARAM_RESCAN_VALUES)\n");
                ++failures;
            } else {
                printf("SUCCESS: state->load() dispatched rescan(CLAP_PARAM_RESCAN_VALUES) to host.\n");
            }

            double d = 0.0, r = 0.0, e = 0.0, s = 0.0, p_val = 0.0, t_val = 0.0, es_val = 0.0, rf_val = -1.0;
            params->get_value(plugin, 1, &d);
            params->get_value(plugin, 2, &r);
            params->get_value(plugin, 3, &e);
            params->get_value(plugin, 4, &s);
            params->get_value(plugin, 5, &p_val);
            params->get_value(plugin, 6, &t_val);
            params->get_value(plugin, 7, &es_val);
            params->get_value(plugin, 8, &rf_val);
            if (fabs(rf_val) > 1e-9) {
                printf("ERROR: reflections switch was not restored (got %f)\n", rf_val);
                ++failures;
            }
            if (fabs(d - pos_m(8.0)) > 1e-9) {
                printf("ERROR: distance was not restored (got %f)\n", d);
                ++failures;
            }
            if (fabs(r - 0.75) > 1e-9) {
                printf("ERROR: rotation was not restored (got %f)\n", r);
                ++failures;
            }
            if (fabs(e - 30.0) > 1e-9) {
                printf("ERROR: elevation was not restored (got %f)\n", e);
                ++failures;
            }
            if (fabs(s - 0.40) > 1e-9) {
                printf("ERROR: space was not restored (got %f)\n", s);
                ++failures;
            }
            if (fabs(p_val - 1.0) > 1e-9) {
                printf("ERROR: test pulse was not restored (got %f)\n", p_val);
                ++failures;
            }
            if (fabs(t_val - 0.85) > 1e-9) {
                printf("ERROR: test tone was not restored (got %f)\n", t_val);
                ++failures;
            }
            if (fabs(es_val - 1.15) > 1e-9) {
                printf("ERROR: ear scale was not restored (got %f)\n", es_val);
                ++failures;
            }
            if (fabs(d - pos_m(8.0)) <= 1e-9 && fabs(r - 0.75) <= 1e-9 && fabs(e - 30.0) <= 1e-9 &&
                fabs(s - 0.40) <= 1e-9 && fabs(p_val - 1.0) <= 1e-9 && fabs(t_val - 0.85) <= 1e-9 &&
                fabs(es_val - 1.15) <= 1e-9 && fabs(rf_val) <= 1e-9) {
                printf("SUCCESS: state round-trip preserved all 8 parameters.\n");
            }

            /* Hostile state stream tests: reject bad magic, invalid version, truncated header/payload, NULL stream */
            {
                int hostile_failures = 0;
                double baseline_vals[8];
                for (uint32_t pi = 1; pi <= 8; ++pi) {
                    params->get_value(plugin, pi, &baseline_vals[pi - 1]);
                }

                #define ASSERT_HOSTILE_REJECTED(stream_ptr, test_desc) do { \
                    if (state->load(plugin, (stream_ptr))) { \
                        printf("ERROR: state->load() accepted " test_desc "\n"); \
                        ++hostile_failures; \
                        ++failures; \
                    } \
                    for (uint32_t pi = 1; pi <= 8; ++pi) { \
                        double cur_v = 0.0; \
                        params->get_value(plugin, pi, &cur_v); \
                        if (cur_v != baseline_vals[pi - 1]) { \
                            printf("ERROR: " test_desc " mutated parameter %u (was %f, now %f)\n", \
                                   pi, baseline_vals[pi - 1], cur_v); \
                            ++hostile_failures; \
                            ++failures; \
                        } \
                    } \
                } while (0)

                /* 1. Bad magic: "NOPE" */
                MemStream bad_magic;
                memset(&bad_magic, 0, sizeof(bad_magic));
                memcpy(bad_magic.data, "NOPE", 4);
                uint32_t v = HRTF_STATE_VERSION;
                memcpy(bad_magic.data + 4, &v, sizeof(v));
                bad_magic.size = 64;
                clap_istream_t is_bm = { &bad_magic, mem_read };
                ASSERT_HOSTILE_REJECTED(&is_bm, "stream with invalid magic 'NOPE'");

                /* 2. Invalid version: "HRTF" with version 99 */
                MemStream bad_ver;
                memset(&bad_ver, 0, sizeof(bad_ver));
                memcpy(bad_ver.data, "HRTF", 4);
                uint32_t bad_v = 99;
                memcpy(bad_ver.data + 4, &bad_v, sizeof(bad_v));
                bad_ver.size = 64;
                clap_istream_t is_bv = { &bad_ver, mem_read };
                ASSERT_HOSTILE_REJECTED(&is_bv, "stream with invalid version 99");

                /* 3. Truncated version read: 4 bytes magic "HRTF" + 2 bytes version */
                MemStream trunc_ver;
                memset(&trunc_ver, 0, sizeof(trunc_ver));
                memcpy(trunc_ver.data, "HRTF", 4);
                uint16_t short_v = (uint16_t)HRTF_STATE_VERSION;
                memcpy(trunc_ver.data + 4, &short_v, sizeof(short_v));
                trunc_ver.size = 6;
                clap_istream_t is_tv = { &trunc_ver, mem_read };
                ASSERT_HOSTILE_REJECTED(&is_tv, "stream with truncated version (6 bytes)");

                /* 4. Truncated header: only 3 bytes */
                MemStream trunc_hdr;
                memset(&trunc_hdr, 0, sizeof(trunc_hdr));
                memcpy(trunc_hdr.data, "HRT", 3);
                trunc_hdr.size = 3;
                clap_istream_t is_th = { &trunc_hdr, mem_read };
                ASSERT_HOSTILE_REJECTED(&is_th, "truncated 3-byte header");

                /* 5. Incomplete parameter payload: 8-byte valid header + only 16 bytes (2 doubles instead of 7) */
                MemStream trunc_payload;
                memset(&trunc_payload, 0, sizeof(trunc_payload));
                memcpy(trunc_payload.data, "HRTF", 4);
                memcpy(trunc_payload.data + 4, &v, sizeof(v));
                trunc_payload.size = 24; /* 8 + 16 */
                clap_istream_t is_tp = { &trunc_payload, mem_read };
                ASSERT_HOSTILE_REJECTED(&is_tp, "incomplete parameter payload (24 bytes)");

                /* 6. Null stream */
                ASSERT_HOSTILE_REJECTED(NULL, "NULL stream");

                #undef ASSERT_HOSTILE_REJECTED

                if (hostile_failures == 0) {
                    printf("SUCCESS: Hostile state streams rejected without mutating parameters (bad magic, invalid version, truncated header/payload, NULL stream).\n");
                }
            }

            /* The restored state switched the room off; the tests below expect it on. */
            flush_param(plugin, params, 8, 1.0);

            /* Test near-field ITD delay without aliasing: d = 5 cm, ear_scale = 1.30, 90 deg right */
            {
                flush_param(plugin, params, 1, pos_m(0.05)); /* 5 cm distance */
                flush_param(plugin, params, 2, 0.25); /* 90 deg right */
                flush_param(plugin, params, 3, 0.0);  /* horizontal */
                flush_param(plugin, params, 4, 0.0);  /* anechoic to isolate ITD */
                flush_param(plugin, params, 5, 0.0);  /* Test pulse OFF */
                flush_param(plugin, params, 7, 1.30); /* 130% ear scale */

                /* reset() snaps the smoothers to their targets. Settling by
                   waiting alone left the 50 ms distance smoother ~1.4% short
                   of 5 cm, which moved the arrival from ~72 to ~63 samples. */
                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                plugin->process(plugin, &process); /* apply the flushed values to the core */
                plugin->reset(plugin);
                for (int b = 0; b < 4; ++b) plugin->process(plugin, &process);

                /* Feed single-sample unit impulse */
                in_buf[0] = 1.0f;
                plugin->process(plugin, &process);

                /* Locate peak arrival in left (far) ear */
                uint32_t far_peak_idx = 0;
                float far_peak_val = 0.0f;
                for (uint32_t i = 0; i < N; ++i) {
                    if (fabsf(out_l[i]) > far_peak_val) {
                        far_peak_val = fabsf(out_l[i]);
                        far_peak_idx = i;
                    }
                }

                /* Settled near-field ITD: 0.00070 * 1.600933 * 1.30 * 48000 + 2 = 71.9 samples.
                   An undersized delay line would wrap it to a few samples. */
                if (far_peak_idx >= 69 && far_peak_idx <= 75) {
                    printf("SUCCESS: Near-field ITD delay is %u samples (expected 72 +/- 3, correctly un-aliased).\n",
                           far_peak_idx);
                } else {
                    printf("ERROR: Near-field ITD mismatch! Peak arrived at sample %u instead of 72 +/- 3\n",
                           far_peak_idx);
                    ++failures;
                }
            }

            /* Test intermediate angle (30 deg right) ITD to validate Woodworth spherical model vs naive sine */
            {
                /* 30 deg rotation: 30 / 360 = 1/12 ≈ 0.083333 */
                flush_param(plugin, params, 1, pos_m(2.0));          /* 2.0 m distance (far field, nf_scale = 1.0) */
                flush_param(plugin, params, 2, 30.0 / 360.0); /* 30 deg azimuth (s = 0.50) */
                flush_param(plugin, params, 3, 0.0);           /* horizontal plane */
                flush_param(plugin, params, 4, 0.0);           /* anechoic */
                flush_param(plugin, params, 5, 0.0);           /* Test pulse OFF */
                flush_param(plugin, params, 7, 1.0);           /* 100% standard ear scale */

                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                for (int b = 0; b < 40; ++b) plugin->process(plugin, &process);

                /* Feed single-sample unit impulse */
                in_buf[0] = 1.0f;
                plugin->process(plugin, &process);

                /* Locate arrivals in near (R) and far (L) ears */
                uint32_t near_peak_idx = 0, far_peak_idx = 0;
                float near_max = 0.0f, far_max = 0.0f;
                for (uint32_t i = 0; i < N; ++i) {
                    if (fabsf(out_r[i]) > near_max) { near_max = fabsf(out_r[i]); near_peak_idx = i; }
                    if (fabsf(out_l[i]) > far_max)  { far_max = fabsf(out_l[i]); far_peak_idx = i; }
                }

                /* Under Woodworth spherical ray-tracing at 30 deg (s = 0.5):
                   woodworth_scale = (sin(pi/6) + pi/6) / (1 + 0.5 * pi) = 1.0236 / 2.5708 ≈ 0.3982
                   itd = 0.00070 * 0.3982 * 48000 ≈ 13.38 samples.
                   Near ear delay = 2 (guard). Far ear delay = 2 + 13.38 ≈ 15.38 samples.
                   Difference = far_peak_idx - near_peak_idx ≈ 13..14 samples.
                   Under naive sine model (sin(30 deg) = 0.5000):
                   sine_itd = 0.00070 * 0.5 * 48000 = 16.80 samples (difference ≈ 17 samples). */
                uint32_t itd_diff = (far_peak_idx >= near_peak_idx) ? (far_peak_idx - near_peak_idx) : 0;
                if (itd_diff >= 13 && itd_diff <= 15) {
                    printf("SUCCESS: Intermediate angle (30 deg) ITD matches Woodworth model (%u samples, expected ~13-14; naive sine would be ~17).\n",
                           itd_diff);
                } else {
                    printf("ERROR: Intermediate angle (30 deg) ITD mismatch! Measured %u samples (expected 13..15 for Woodworth model, naive sine ~17).\n",
                           itd_diff);
                    ++failures;
                }
            }

            /* Test Test Pulse "OFF" state: the generator must actually be
               sounding first, then fall silent once disabled. */
            {
                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                flush_param(plugin, params, 5, 1.0); /* Enable Test Pulse */
                float on_sum = 0.0f;
                for (int b = 0; b < 10; ++b) {
                    plugin->process(plugin, &process);
                    for (uint32_t i = 0; i < N; ++i) on_sum += fabsf(out_l[i]) + fabsf(out_r[i]);
                }
                if (on_sum < 0.01f) {
                    printf("ERROR: Test pulse produced no audio before the OFF check (sum=%f)\n", on_sum);
                    ++failures;
                }

                flush_param(plugin, params, 5, 0.0); /* Disable Test Pulse */
                for (int b = 0; b < 20; ++b) plugin->process(plugin, &process);
                float silent_sum = 0.0f;
                for (uint32_t i = 0; i < N; ++i) silent_sum += fabsf(out_l[i]) + fabsf(out_r[i]);
                if (silent_sum < 1e-11f) {
                    printf("SUCCESS: Test pulse generator OFF state confirmed (silent output: sum=%e).\n",
                           silent_sum);
                } else {
                    printf("ERROR: Test pulse generator stuck ON when disabled (sum=%f)\n", silent_sum);
                    ++failures;
                }
            }

            /* Test internal pulse generation with silent input */
            for (uint32_t i = 0; i < N; ++i) {
                in_buf[i] = 0.0f;
                out_l[i] = 0.0f;
                out_r[i] = 0.0f;
            }
            flush_param(plugin, params, 5, 1.0); /* Enable Test Pulse */
            flush_param(plugin, params, 6, 0.5); /* 50% pink noise */
            plugin->process(plugin, &process);
            float pulse_sum = 0.0f;
            for (uint32_t i = 0; i < N; ++i) {
                pulse_sum += fabsf(out_l[i]) + fabsf(out_r[i]);
            }
            if (pulse_sum > 0.01f) {
                printf("SUCCESS: Test pulse generator synthesised audio from silent input (sum=%f).\n", pulse_sum);
            } else {
                printf("ERROR: Test pulse generator failed to synthesise audio (sum=%f)\n", pulse_sum);
                ++failures;
            }

            /* Dogfood internal test pulse through 3D rotation: rotate to 90 deg right */
            flush_param(plugin, params, 2, 0.25);
            for (int b = 0; b < 6; ++b) {
                plugin->process(plugin, &process);
            }
            float rot_pulse_l = 0.0f, rot_pulse_r = 0.0f;
            for (uint32_t i = 0; i < N; ++i) {
                rot_pulse_l += fabsf(out_l[i]);
                rot_pulse_r += fabsf(out_r[i]);
            }
            if (rot_pulse_r > rot_pulse_l * 1.3f) {
                printf("SUCCESS: Dogfooded internal test pulse through 3D rotation (Left=%f, Right=%f).\n",
                       rot_pulse_l, rot_pulse_r);
            } else {
                printf("ERROR: Internal test pulse did not bias right ear (Left=%f, Right=%f)\n",
                       rot_pulse_l, rot_pulse_r);
                ++failures;
            }

            /* Dogfood internal test pulse tone morphing: rumble vs crisp with spectral slew delta over 1 beat */
            flush_param(plugin, params, 2, 0.0); /* centre */
            flush_param(plugin, params, 5, 1.0); /* Enable pulse */
            flush_param(plugin, params, 6, 0.0); /* sub rumble */
            float rumble_deltas = 0.0f;
            for (int b = 0; b < 100; ++b) {
                plugin->process(plugin, &process);
                for (uint32_t i = 1; i < N; ++i) {
                    rumble_deltas += fabsf(out_l[i] - out_l[i-1]) + fabsf(out_r[i] - out_r[i-1]);
                }
            }

            flush_param(plugin, params, 6, 1.0); /* crisp transient */
            float crisp_deltas = 0.0f;
            for (int b = 0; b < 100; ++b) {
                plugin->process(plugin, &process);
                for (uint32_t i = 1; i < N; ++i) {
                    crisp_deltas += fabsf(out_l[i] - out_l[i-1]) + fabsf(out_r[i] - out_r[i-1]);
                }
            }

            if (crisp_deltas > rumble_deltas * 2.0f) {
                printf("SUCCESS: Dogfooded internal test tone spectrum shift (crisp delta=%f > rumble delta=%f).\n",
                       crisp_deltas, rumble_deltas);
            } else {
                printf("ERROR: Test tone did not increase transient sharpness (rumble=%f, crisp=%f)\n",
                       rumble_deltas, crisp_deltas);
                ++failures;
            }

            /* Worst-case limiter transparency and active compression tests. The
               plugin now runs 2 dB under unity at 2 m, so a full-scale input meets
               the limiter when it lands on a filter boost; the transparency check
               drives sustained sines at -8 dBFS, a typical mix peak level.
               Worst-case geometry: d = 0.05 m (+2.88 dB distance gain, +7.4 dB pinna presence)
               + 100% room reflections (space = 1.0). */
            {
                flush_param(plugin, params, 1, pos_m(0.05)); /* 5 cm distance */
                flush_param(plugin, params, 2, 0.0);  /* 0 deg rotation (front) */
                flush_param(plugin, params, 3, 0.0);  /* 0 deg elevation */
                flush_param(plugin, params, 4, 1.0);  /* 100% space (reflections) */
                flush_param(plugin, params, 5, 0.0);  /* internal test pulse OFF */
                flush_param(plugin, params, 7, 1.0);  /* 100% ear scale */

                const float kLimiterKnee = 0.89125f; /* -1.0 dBFS */
                const float kSineInput = 0.398107f;  /* -8 dBFS */

                /* Settle smoothers */
                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                for (int b = 0; b < 20; ++b) plugin->process(plugin, &process);

                /* Frequency sweep of -8 dBFS sine waves:
                   1000 Hz, 2500 Hz, 3900 Hz, 3973 Hz (settled) plus a cold-start transient probe */
                const double sweep_freqs[] = { 1000.0, 2500.0, 3900.0, 3973.0 };
                const size_t num_sweep_freqs = sizeof(sweep_freqs) / sizeof(sweep_freqs[0]);
                float sweep_max_peak = 0.0f;

                for (size_t f = 0; f < num_sweep_freqs; ++f) {
                    double freq = sweep_freqs[f];
                    double phase_acc = 0.0;
                    const double phase_inc = 2.0 * 3.14159265358979323846 * freq / 48000.0;
                    for (int b = 0; b < 50; ++b) {
                        for (uint32_t i = 0; i < N; ++i) {
                            in_buf[i] = kSineInput * (float)sin(phase_acc);
                            phase_acc += phase_inc;
                            if (phase_acc >= 2.0 * 3.14159265358979323846)
                                phase_acc -= 2.0 * 3.14159265358979323846;
                        }
                        plugin->process(plugin, &process);
                        for (uint32_t i = 0; i < N; ++i) {
                            float al = fabsf(out_l[i]);
                            float ar = fabsf(out_r[i]);
                            if (al > sweep_max_peak) sweep_max_peak = al;
                            if (ar > sweep_max_peak) sweep_max_peak = ar;
                        }
                    }
                }

                /* Cold-start transient probe: clear history with reset, then immediately feed 3973 Hz */
                plugin->reset(plugin);
                {
                    double phase_acc = 0.0;
                    const double phase_inc = 2.0 * 3.14159265358979323846 * 3973.0 / 48000.0;
                    for (int b = 0; b < 10; ++b) {
                        for (uint32_t i = 0; i < N; ++i) {
                            in_buf[i] = kSineInput * (float)sin(phase_acc);
                            phase_acc += phase_inc;
                            if (phase_acc >= 2.0 * 3.14159265358979323846)
                                phase_acc -= 2.0 * 3.14159265358979323846;
                        }
                        plugin->process(plugin, &process);
                        for (uint32_t i = 0; i < N; ++i) {
                            float al = fabsf(out_l[i]);
                            float ar = fabsf(out_r[i]);
                            if (al > sweep_max_peak) sweep_max_peak = al;
                            if (ar > sweep_max_peak) sweep_max_peak = ar;
                        }
                    }
                }

                if (sweep_max_peak < kLimiterKnee) {
                    float margin_pct = (1.0f - sweep_max_peak / kLimiterKnee) * 100.0f;
                    printf("SUCCESS: Worst-case limiter transparency verified for -8 dBFS sines (peak=%f < knee 0.89125 [-1.0 dBFS], margin=%.1f%%, limiter idle).\n",
                           sweep_max_peak, margin_pct);
                } else {
                    printf("ERROR: Output peak (%f) engaged soft-limiter knee (%f) under -8 dBFS input!\n",
                           sweep_max_peak, kLimiterKnee);
                    ++failures;
                }

                /* Active limiter exercise: drive with deliberately hot input (+6 dBFS, amplitude 2.0).
                   Exercises soft_limit() waveshaper compression.
                   Falsification check: If soft_limit(x) returned x, peak would be > 1.6! */
                float hot_max_peak = 0.0f;
                double hot_phase = 0.0;
                const double hot_inc = 2.0 * 3.14159265358979323846 * 3973.0 / 48000.0;
                for (int b = 0; b < 50; ++b) {
                    for (uint32_t i = 0; i < N; ++i) {
                        in_buf[i] = 2.0f * (float)sin(hot_phase);
                        hot_phase += hot_inc;
                        if (hot_phase >= 2.0 * 3.14159265358979323846)
                            hot_phase -= 2.0 * 3.14159265358979323846;
                    }
                    plugin->process(plugin, &process);
                    for (uint32_t i = 0; i < N; ++i) {
                        float al = fabsf(out_l[i]);
                        float ar = fabsf(out_r[i]);
                        if (al > hot_max_peak) hot_max_peak = al;
                        if (ar > hot_max_peak) hot_max_peak = ar;
                    }
                }

                if (hot_max_peak > kLimiterKnee && hot_max_peak <= 1.000000f) {
                    printf("SUCCESS: Active limiter exercise verified: hot +6 dBFS input drove waveshaper past knee (peak=%f > 0.89125) while strictly observing ceiling <= 1.000000.\n",
                           hot_max_peak);
                } else {
                    printf("ERROR: Active limiter failure on +6 dBFS input! Peak was %f (expected > 0.89125 and <= 1.000000)\n",
                           hot_max_peak);
                    ++failures;
                }
            }
        }
    }

    /* Regression tests for wrapper and DSP defects found in review. */
    if (params) {
        const double two_pi = 2.0 * 3.14159265358979323846;

        /* Neutral, reproducible starting point for each comparison. */
        #define NEUTRAL_PARAMS() do { \
            flush_param(plugin, params, 1, pos_m(2.0)); \
            flush_param(plugin, params, 2, 0.1); \
            flush_param(plugin, params, 3, 0.0); \
            flush_param(plugin, params, 4, 0.3); \
            flush_param(plugin, params, 5, 0.0); \
            flush_param(plugin, params, 7, 1.0); \
            flush_param(plugin, params, 8, 1.0); \
            plugin->reset(plugin); \
        } while (0)

        /* 1. Stereo input port, downmixed so the right channel is not dropped. */
        {
            const clap_plugin_audio_ports_t *ports =
                (const clap_plugin_audio_ports_t *)plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
            clap_audio_port_info_t pinfo;
            if (!ports || !ports->get(plugin, 0, true, &pinfo) || pinfo.channel_count != 2) {
                printf("ERROR: CLAP input port is not stereo\n");
                ++failures;
            }

            static float x[256], half_x[256], zeros[256];
            static float ref_l[256], ref_r[256];
            for (uint32_t i = 0; i < N; ++i) {
                x[i] = (float)(0.8 * sin(two_pi * 700.0 * i / 48000.0));
                half_x[i] = 0.5f * x[i];
                zeros[i] = 0.0f;
            }

            clap_audio_buffer_t saved_in = in_audio;

            /* Reference: mono buffer carrying 0.5 * x. */
            NEUTRAL_PARAMS();
            float *mono_ptrs[1] = { half_x };
            in_audio.data32 = mono_ptrs;
            in_audio.channel_count = 1;
            plugin->process(plugin, &process);
            memcpy(ref_l, out_l, sizeof(ref_l));
            memcpy(ref_r, out_r, sizeof(ref_r));

            /* Stereo with the signal only on the right: 0.5 * (0 + x). */
            NEUTRAL_PARAMS();
            float *stereo_ptrs[2] = { zeros, x };
            in_audio.data32 = stereo_ptrs;
            in_audio.channel_count = 2;
            plugin->process(plugin, &process);

            if (memcmp(ref_l, out_l, sizeof(ref_l)) == 0 && memcmp(ref_r, out_r, sizeof(ref_r)) == 0) {
                printf("SUCCESS: Stereo input is downmixed (right-only input renders identically to its mono sum).\n");
            } else {
                printf("ERROR: Stereo input downmix mismatch (right channel dropped or mis-scaled)\n");
                ++failures;
            }
            in_audio = saved_in;
        }

        for (uint32_t i = 0; i < N; ++i)
            in_buf[i] = (float)(0.5 * sin(two_pi * 1000.0 * i / 48000.0));

        /* 2. In-process events take effect at their sample offset. */
        {
            static float ref_l[256];
            NEUTRAL_PARAMS();
            plugin->process(plugin, &process);
            memcpy(ref_l, out_l, sizeof(ref_l));

            NEUTRAL_PARAMS();
            clap_event_param_value_t evs[2] = {
                make_param_event(128, 2, 0.25),
                make_param_event(300, 1, pos_m(7.0)) /* stamped beyond the 256-frame block */
            };
            EventArray arr = { evs, 2 };
            clap_input_events_t list = { &arr, array_events_size, array_events_get };
            process.in_events = &list;
            plugin->process(plugin, &process);
            process.in_events = NULL;

            uint32_t first_diff = N;
            for (uint32_t i = 0; i < N; ++i) {
                if (out_l[i] != ref_l[i]) { first_diff = i; break; }
            }
            double dist = 0.0;
            params->get_value(plugin, 1, &dist);
            if (first_diff == 128) {
                printf("SUCCESS: CLAP in-process event applied at its exact sample offset (128).\n");
            } else {
                printf("ERROR: CLAP event at offset 128 first changed the output at sample %u\n", first_diff);
                ++failures;
            }
            if (fabs(dist - pos_m(7.0)) < 1e-9) {
                printf("SUCCESS: CLAP event stamped beyond the block was applied, not dropped.\n");
            } else {
                printf("ERROR: CLAP event stamped beyond the block was dropped (distance=%f)\n", dist);
                ++failures;
            }

            /* Events delivered with an inactive output port must still land. */
            clap_event_param_value_t ev_inactive = make_param_event(0, 3, 45.0);
            EventArray arr2 = { &ev_inactive, 1 };
            clap_input_events_t list2 = { &arr2, array_events_size, array_events_get };
            clap_audio_buffer_t saved_out = out_audio;
            out_audio.data32 = NULL;
            process.in_events = &list2;
            plugin->process(plugin, &process);
            process.in_events = NULL;
            out_audio = saved_out;
            double elev = 0.0;
            params->get_value(plugin, 3, &elev);
            if (fabs(elev - 45.0) < 1e-9) {
                printf("SUCCESS: CLAP events are applied even when the output port is inactive.\n");
            } else {
                printf("ERROR: CLAP event lost with an inactive output port (elevation=%f)\n", elev);
                ++failures;
            }
        }

        /* 3. Rotation text round-trips for small angles ("0.4 deg" used to parse as 144 deg). */
        {
            const double vals[] = { 0.0005, 0.001, 0.0027, 0.1, 0.5, 0.9 };
            int ok = 1;
            for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); ++k) {
                char buf[64];
                double back = -1.0;
                if (!params->value_to_text(plugin, 2, vals[k], buf, sizeof(buf)) ||
                    !params->text_to_value(plugin, 2, buf, &back)) {
                    ok = 0;
                    printf("ERROR: rotation text conversion failed for %f\n", vals[k]);
                    continue;
                }
                double d = fabs(back - vals[k]);
                if (d > 0.5) d = 1.0 - d; /* periodic */
                if (d > 0.0005) {
                    ok = 0;
                    printf("ERROR: rotation %f -> \"%s\" -> %f\n", vals[k], buf, back);
                }
            }
            double typed = -1.0;
            if (!params->text_to_value(plugin, 2, "1 deg", &typed) || fabs(typed - 1.0 / 360.0) > 1e-12) {
                ok = 0;
                printf("ERROR: \"1 deg\" parsed to phase %f\n", typed);
            }
            if (ok) {
                printf("SUCCESS: Rotation text round-trips at small angles and explicit 'deg' is always degrees.\n");
            } else {
                ++failures;
            }
        }

        /* 4. Render is independent of the host's block size. */
        {
            enum { LEN = 1024 };
            static float src[LEN], a_l[LEN], a_r[LEN], b_l[LEN], b_r[LEN];
            for (uint32_t i = 0; i < LEN; ++i)
                src[i] = (float)(0.5 * sin(two_pi * 3000.0 * i / 48000.0));

            clap_audio_buffer_t saved_in = in_audio, saved_out = out_audio;
            const uint32_t sizes[2] = { 256, 7 };
            for (int pass = 0; pass < 2; ++pass) {
                NEUTRAL_PARAMS();
                flush_param(plugin, params, 2, 0.35); /* smoother in motion */
                flush_param(plugin, params, 3, 40.0);
                float *dl = pass ? b_l : a_l, *dr = pass ? b_r : a_r;
                for (uint32_t pos = 0; pos < LEN; ) {
                    uint32_t n = sizes[pass];
                    if (pos + n > LEN) n = LEN - pos;
                    float *ip[1] = { src + pos };
                    float *op[2] = { dl + pos, dr + pos };
                    in_audio.data32 = ip; in_audio.channel_count = 1;
                    out_audio.data32 = op;
                    process.frames_count = n;
                    plugin->process(plugin, &process);
                    pos += n;
                }
            }
            process.frames_count = N;
            in_audio = saved_in;
            out_audio = saved_out;

            if (memcmp(a_l, b_l, sizeof(a_l)) == 0 && memcmp(a_r, b_r, sizeof(a_r)) == 0) {
                printf("SUCCESS: Render is bit-identical for 256-frame and 7-frame host blocks.\n");
            } else {
                printf("ERROR: Render depends on host block size\n");
                ++failures;
            }
        }

        /* 5. A NaN input sample must not latch the filters. */
        {
            NEUTRAL_PARAMS();
            float saved = in_buf[10];
            in_buf[10] = NAN;
            plugin->process(plugin, &process);
            in_buf[10] = saved;
            plugin->process(plugin, &process);
            int finite = 1;
            float energy = 0.0f;
            for (uint32_t i = 0; i < N; ++i) {
                if (!isfinite(out_l[i]) || !isfinite(out_r[i])) finite = 0;
                energy += fabsf(out_l[i]) + fabsf(out_r[i]);
            }
            if (finite && energy > 0.01f) {
                printf("SUCCESS: Output recovers immediately after a NaN input sample.\n");
            } else {
                printf("ERROR: NaN input sample poisoned the output (finite=%d, energy=%f)\n", finite, energy);
                ++failures;
            }
        }

        /* 6. Reflection taps glide during rotation instead of stepping whole samples. */
        {
            NEUTRAL_PARAMS();
            flush_param(plugin, params, 2, 0.0);
            flush_param(plugin, params, 4, 1.0); /* full room */
            plugin->reset(plugin);

            const double w = two_pi * 5000.0 / 48000.0;
            const double c2 = 2.0 * cos(w);
            double worst = 0.0, peak = 0.0;
            float y1 = 0.0f, y2 = 0.0f;
            uint64_t n_abs = 0;
            const int blocks = 188; /* ~1 s: one full turn */
            for (int b = 0; b < blocks; ++b) {
                flush_param(plugin, params, 2, (double)b / blocks);
                for (uint32_t i = 0; i < N; ++i, ++n_abs)
                    in_buf[i] = (float)(0.5 * sin(w * (double)n_abs));
                plugin->process(plugin, &process);
                for (uint32_t i = 0; i < N; ++i) {
                    if (b >= 20) {
                        double r = fabs((double)out_l[i] - c2 * y1 + y2);
                        if (r > worst) worst = r;
                        if (fabs((double)out_l[i]) > peak) peak = fabs((double)out_l[i]);
                    }
                    y2 = y1;
                    y1 = out_l[i];
                }
            }
            /* Residual relative to the output peak: a whole-sample tap jump on
               this signal leaves ~8% of the peak; a smooth glide stays well under 1%. */
            if (peak > 0.0 && worst / peak < 0.02) {
                printf("SUCCESS: Room reflections glide smoothly under rotation (max residual %.2f%% of peak).\n", 100.0 * worst / peak);
            } else {
                printf("ERROR: Room reflections step during rotation (max residual %.2f%% of peak)\n", 100.0 * worst / peak);
                ++failures;
            }
        }

        /* 7. Output level: -2 dB at the default 2 m, inverse distance beyond,
              and never louder than the input in either ear. Uses its own
              pink noise (a different seed from the calibration sweep). */
        {
            /* Pink noise needs long windows for a stable level: ~2 s for the
               calibration points, ~1.4 s per sweep position. */
            enum { SETTLE = 8, BLOCKS = 256, ANCHOR_BLOCKS = 400 };
            static float noise[(SETTLE + ANCHOR_BLOCKS) * 256];
            make_pink_noise(noise, (SETTLE + ANCHOR_BLOCKS) * 256, 0xC0FFEEu, 0.05f);

            double ll, lr;
            double level_at[4];
            const double dists[4] = { 2.0, 4.0, 8.0, 20.0 };
            for (int k = 0; k < 4; ++k) {
                NEUTRAL_PARAMS();
                flush_param(plugin, params, 1, pos_m(dists[k]));
                flush_param(plugin, params, 2, 0.0);
                flush_param(plugin, params, 4, 0.15);
                plugin->reset(plugin);
                measure_levels(plugin, &process, in_buf, out_l, out_r, noise, SETTLE, ANCHOR_BLOCKS, &ll, &lr);
                level_at[k] = 0.5 * (ll + lr);
            }
            if (fabs(level_at[0] + 2.0) <= 0.2) {
                printf("SUCCESS: Default position (2 m, front, 15%% Space) sits %.2f dB relative to the input (target -2 dB).\n", level_at[0]);
            } else {
                printf("ERROR: Default position level is %.2f dB, expected -2.0 +/- 0.2 dB\n", level_at[0]);
                ++failures;
            }
            const double step4 = level_at[1] - level_at[0];
            if (step4 <= -5.0 && step4 >= -6.5 && level_at[2] < level_at[1] && level_at[3] < level_at[2]) {
                printf("SUCCESS: Level fans out by inverse distance beyond 2 m (4 m %.2f dB, 8 m %.2f dB, 20 m %.2f dB).\n",
                       level_at[1], level_at[2], level_at[3]);
            } else {
                printf("ERROR: Far-field level curve wrong (2 m %.2f, 4 m %.2f, 8 m %.2f, 20 m %.2f dB)\n",
                       level_at[0], level_at[1], level_at[2], level_at[3]);
                ++failures;
            }

            /* Closer than 2 m the level may rise a little but never falls. */
            NEUTRAL_PARAMS();
            flush_param(plugin, params, 1, pos_m(0.05));
            flush_param(plugin, params, 2, 0.0);
            flush_param(plugin, params, 4, 0.15);
            plugin->reset(plugin);
            measure_levels(plugin, &process, in_buf, out_l, out_r, noise, SETTLE, ANCHOR_BLOCKS, &ll, &lr);
            const double near_level = 0.5 * (ll + lr);
            if (near_level >= level_at[0] - 0.1 && near_level <= 0.0) {
                printf("SUCCESS: At 5 cm the level is %.2f dB (no quieter than at 2 m, never above the input).\n", near_level);
            } else {
                printf("ERROR: At 5 cm the level is %.2f dB (2 m is %.2f dB)\n", near_level, level_at[0]);
                ++failures;
            }

            /* Never louder: sweep distance, direction, ear scale and Space. */
            const double sweep_d[6] = { 0.05, 0.25, 1.0, 1.5, 2.0, 3.0 };
            const double sweep_el[2] = { 0.0, 30.0 };
            const double sweep_es[2] = { 0.7, 1.3 };
            const double sweep_sp[2] = { 0.15, 1.0 };
            double loudest = -1e9;
            double at_d = 0, at_az = 0, at_el = 0, at_es = 0, at_sp = 0;
            for (int a = 0; a < 6; ++a)
                for (int az = 0; az <= 180; az += 15) /* left side mirrors the right */
                    for (int e = 0; e < 2; ++e)
                        for (int x = 0; x < 2; ++x)
                            for (int y = 0; y < 2; ++y) {
                                flush_param(plugin, params, 1, pos_m(sweep_d[a]));
                                flush_param(plugin, params, 2, az / 360.0);
                                flush_param(plugin, params, 3, sweep_el[e]);
                                flush_param(plugin, params, 7, sweep_es[x]);
                                flush_param(plugin, params, 4, sweep_sp[y]);
                                plugin->reset(plugin);
                                measure_levels(plugin, &process, in_buf, out_l, out_r, noise, SETTLE, BLOCKS, &ll, &lr);
                                const double m = ll > lr ? ll : lr;
                                if (m > loudest) {
                                    loudest = m; at_d = sweep_d[a]; at_az = az; at_el = sweep_el[e];
                                    at_es = sweep_es[x]; at_sp = sweep_sp[y];
                                }
                            }
            if (loudest <= 0.0) {
                printf("SUCCESS: No ear is ever louder than the input (loudest %.2f dB at %.2f m, %.0f deg, elev %.0f, ear %.0f%%, space %.0f%%).\n",
                       loudest, at_d, at_az, at_el, at_es * 100.0, at_sp * 100.0);
            } else {
                printf("ERROR: An ear is %.2f dB louder than the input at %.2f m, %.0f deg, elev %.0f, ear %.0f%%, space %.0f%%\n",
                       loudest, at_d, at_az, at_el, at_es * 100.0, at_sp * 100.0);
                ++failures;
            }
        }

        /* 8. Logarithmic distance taper: half the travel covers 5 cm to 1 m. */
        {
            char buf[64] = {0};
            double v_cm = -1.0, v_far = -1.0;
            params->value_to_text(plugin, 1, 0.5, buf, sizeof(buf));
            const int text_ok = strcmp(buf, "1 m") == 0;
            const int cm_ok = params->text_to_value(plugin, 1, "5 cm", &v_cm) && fabs(v_cm) < 1e-12;
            const int far_ok = params->text_to_value(plugin, 1, "20 m", &v_far) && fabs(v_far - 1.0) < 1e-12;
            if (text_ok && cm_ok && far_ok) {
                printf("SUCCESS: Distance taper is logarithmic (mid-travel reads \"%s\", 5 cm and 20 m are the ends).\n", buf);
            } else {
                printf("ERROR: Distance taper wrong (mid-travel \"%s\", 5 cm -> %f, 20 m -> %f)\n", buf, v_cm, v_far);
                ++failures;
            }
        }

        /* 9. Reflections switch: Off renders exactly like Space 0, and
              switching glides instead of stepping. */
        {
            static float ref_l[256];
            for (uint32_t i = 0; i < N; ++i)
                in_buf[i] = (float)(0.5 * sin(two_pi * 1000.0 * i / 48000.0));

            NEUTRAL_PARAMS();
            flush_param(plugin, params, 4, 0.0);
            plugin->reset(plugin);
            for (int b = 0; b < 12; ++b) plugin->process(plugin, &process);
            memcpy(ref_l, out_l, sizeof(ref_l));

            NEUTRAL_PARAMS();
            flush_param(plugin, params, 4, 1.0);
            flush_param(plugin, params, 8, 0.0);
            plugin->reset(plugin);
            for (int b = 0; b < 12; ++b) plugin->process(plugin, &process);
            if (memcmp(ref_l, out_l, sizeof(ref_l)) == 0) {
                printf("SUCCESS: Reflections Off renders identically to Space 0.\n");
            } else {
                printf("ERROR: Reflections Off does not remove the room\n");
                ++failures;
            }

            /* Toggle the room on and off during a steady 5 kHz tone. */
            NEUTRAL_PARAMS();
            flush_param(plugin, params, 4, 1.0);
            plugin->reset(plugin);
            const double w = two_pi * 5000.0 / 48000.0;
            const double c2 = 2.0 * cos(w);
            double worst = 0.0, peak = 0.0;
            float y1 = 0.0f, y2 = 0.0f;
            uint64_t n_abs = 0;
            for (int b = 0; b < 120; ++b) {
                if (b == 40) flush_param(plugin, params, 8, 0.0);
                if (b == 80) flush_param(plugin, params, 8, 1.0);
                for (uint32_t i = 0; i < N; ++i, ++n_abs)
                    in_buf[i] = (float)(0.5 * sin(w * (double)n_abs));
                plugin->process(plugin, &process);
                for (uint32_t i = 0; i < N; ++i) {
                    if (b >= 20) {
                        const double r = fabs((double)out_l[i] - c2 * y1 + y2);
                        if (r > worst) worst = r;
                        if (fabs((double)out_l[i]) > peak) peak = fabs((double)out_l[i]);
                    }
                    y2 = y1;
                    y1 = out_l[i];
                }
            }
            if (peak > 0.0 && worst / peak < 0.02) {
                printf("SUCCESS: Switching Reflections fades smoothly (max residual %.2f%% of peak).\n", 100.0 * worst / peak);
            } else {
                printf("ERROR: Switching Reflections steps the output (max residual %.2f%% of peak)\n", 100.0 * worst / peak);
                ++failures;
            }
        }

        /* 10. Test Pulse edges are click-free: playback starting in the middle
               of a pulse ramps in like the pulse's own attack, and switching
               the pulse off while it sounds fades out over a few milliseconds.
               A hard edge would put full level into the first millisecond after
               the start, or cut it to silence straight after switching off. */
        {
            for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
            NEUTRAL_PARAMS();
            flush_param(plugin, params, 4, 0.0);
            flush_param(plugin, params, 6, 0.0);  /* low rumble: 250 ms pulses, 12 ms attack */
            flush_param(plugin, params, 5, 1.0);
            plugin->reset(plugin);

            /* Free-running (transport stopped) for 15360 samples: past the
               first 12000-sample pulse, so the generator sits in a gap. */
            for (int b = 0; b < 60; ++b) plugin->process(plugin, &process);

            /* Start playback 0.1 beats into the bar: 50 ms into a pulse. */
            clap_event_transport_t tr;
            memset(&tr, 0, sizeof(tr));
            tr.header.size = sizeof(tr);
            tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            tr.header.type = CLAP_EVENT_TRANSPORT;
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
            tr.tempo = 120.0;
            double beat = 0.1;
            const double beats_per_block = 256.0 * 120.0 / (60.0 * 48000.0);
            process.transport = &tr;

            static float after_start[8 * 256];
            for (int b = 0; b < 8; ++b) {
                tr.song_pos_beats = (clap_beattime)llround(beat * (double)CLAP_BEATTIME_FACTOR);
                plugin->process(plugin, &process);
                memcpy(after_start + b * 256, out_l, 256 * sizeof(float));
                beat += beats_per_block;
            }

            /* Still inside the pulse: switch it off. */
            flush_param(plugin, params, 5, 0.0);
            tr.song_pos_beats = (clap_beattime)llround(beat * (double)CLAP_BEATTIME_FACTOR);
            plugin->process(plugin, &process);
            process.transport = NULL;
            flush_param(plugin, params, 6, 0.5);

            double first_ms = 0.0, steady = 0.0, after_off = 0.0;
            for (int i = 0; i < 48; ++i) first_ms += (double)after_start[i] * after_start[i];
            for (int i = 1024; i < 2048; ++i) steady += (double)after_start[i] * after_start[i];
            for (int i = 0; i < 48; ++i) after_off += (double)out_l[i] * out_l[i];
            first_ms = sqrt(first_ms / 48.0);
            steady = sqrt(steady / 1024.0);
            after_off = sqrt(after_off / 48.0);

            const double start_ratio = steady > 0.0 ? first_ms / steady : 1.0;
            const double stop_ratio = steady > 0.0 ? after_off / steady : 0.0;
            if (steady > 1e-4 && start_ratio < 0.4 && stop_ratio > 0.5) {
                printf("SUCCESS: Test Pulse edges are click-free (first ms after a mid-pulse start at %.0f%% of steady level, first ms after switching off still %.0f%%).\n",
                       100.0 * start_ratio, 100.0 * stop_ratio);
            } else {
                printf("ERROR: Test Pulse edge is abrupt (first ms after start %.0f%%, after switching off %.0f%% of steady level %.4f)\n",
                       100.0 * start_ratio, 100.0 * stop_ratio, steady);
                ++failures;
            }
        }

        #undef NEUTRAL_PARAMS
    }

    /* Multi-sample-rate operation: 44.1, 48, 88.2, 96, 176.4, 192, 384 kHz.
       Tests delay buffer sizing (>512 samples at 384 kHz), Nyquist clamps, Woodworth ITD scaling, and median-plane bit symmetry. */
    {
        const double test_rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0 };
        const size_t num_rates = sizeof(test_rates) / sizeof(test_rates[0]);
        const uint32_t multi_N = 2048;
        float *multi_in = (float *)calloc(multi_N, sizeof(float));
        float *multi_l  = (float *)calloc(multi_N, sizeof(float));
        float *multi_r  = (float *)calloc(multi_N, sizeof(float));
        if (!multi_in || !multi_l || !multi_r) {
            printf("ERROR: Failed to allocate multi-rate test buffers\n");
            free(multi_in); free(multi_l); free(multi_r);
            return 1;
        }
        float *multi_in_ptrs[1] = { multi_in };
        float *multi_out_ptrs[2] = { multi_l, multi_r };

        clap_audio_buffer_t multi_in_audio;
        multi_in_audio.data32 = multi_in_ptrs;
        multi_in_audio.channel_count = 1;
        multi_in_audio.latency = 0;
        multi_in_audio.constant_mask = 0;

        clap_audio_buffer_t multi_out_audio;
        multi_out_audio.data32 = multi_out_ptrs;
        multi_out_audio.channel_count = 2;
        multi_out_audio.latency = 0;
        multi_out_audio.constant_mask = 0;

        clap_process_t multi_proc;
        memset(&multi_proc, 0, sizeof(multi_proc));
        multi_proc.steady_time = 0;
        multi_proc.frames_count = multi_N;
        multi_proc.transport = NULL;
        multi_proc.audio_inputs = &multi_in_audio;
        multi_proc.audio_inputs_count = 1;
        multi_proc.audio_outputs = &multi_out_audio;
        multi_proc.audio_outputs_count = 1;
        multi_proc.in_events = NULL;
        multi_proc.out_events = NULL;

        for (size_t r = 0; r < num_rates; ++r) {
            double sr = test_rates[r];
            plugin->deactivate(plugin);
            if (!plugin->activate(plugin, sr, 32, multi_N)) {
                printf("ERROR: plugin->activate() failed at %0.1f Hz\n", sr);
                ++failures;
                continue;
            }

            /* Test 1: Near-field maximum ITD delay scaling at this sample rate */
            flush_param(plugin, params, 1, pos_m(0.05)); /* 5 cm */
            flush_param(plugin, params, 2, 0.25); /* 90 deg right */
            flush_param(plugin, params, 3, 0.0);
            flush_param(plugin, params, 4, 0.0);
            flush_param(plugin, params, 5, 0.0);
            flush_param(plugin, params, 7, 1.30); /* 130% ear scale */

            /* Settle smoothers */
            for (uint32_t i = 0; i < multi_N; ++i) multi_in[i] = 0.0f;
            for (int b = 0; b < 40; ++b) {
                clap_process_status ps = plugin->process(plugin, &multi_proc);
                if (ps != CLAP_PROCESS_CONTINUE) {
                    printf("ERROR: process() returned %d at %0.1f Hz\n", ps, sr);
                    ++failures;
                }
            }

            /* Unit impulse */
            multi_in[0] = 1.0f;
            clap_process_status ps = plugin->process(plugin, &multi_proc);
            if (ps != CLAP_PROCESS_CONTINUE) {
                printf("ERROR: process() returned %d on impulse at %0.1f Hz\n", ps, sr);
                ++failures;
            }

            /* Check filter stability */
            int stable = 1;
            for (uint32_t i = 0; i < multi_N; ++i) {
                if (!isfinite(multi_l[i]) || !isfinite(multi_r[i])) {
                    stable = 0;
                    break;
                }
            }
            if (!stable) {
                printf("ERROR: Non-finite output detected at %0.1f Hz (filter instability)\n", sr);
                ++failures;
            }

            /* Find peak arrival in far ear (left) */
            uint32_t peak_idx = 0;
            float peak_val = 0.0f;
            for (uint32_t i = 0; i < multi_N; ++i) {
                if (fabsf(multi_l[i]) > peak_val) {
                    peak_val = fabsf(multi_l[i]);
                    peak_idx = i;
                }
            }

            /* Woodworth formula expected arrival:
               ITD at 90 deg lateral, 5 cm near-field (nf_scale = 1.600933), 130% ear scale:
               ITD_s = 0.00070 * 1.600933 * 1.30 = 0.00145685 s.
               Arrival index = round(0.00145685 * sr + 2.0). */
            double expected_samples = 0.00145685 * sr + 2.0;
            uint32_t exp_arrival = (uint32_t)round(expected_samples);
            if (peak_idx >= exp_arrival - 3 && peak_idx <= exp_arrival + 3) {
                printf("SUCCESS: ITD delay scales accurately at %0.1f Hz (arrival sample %u, expected %u +/- 3).\n",
                       sr, peak_idx, exp_arrival);
            } else {
                printf("ERROR: ITD delay mismatch at %0.1f Hz! Peak at sample %u, expected %u +/- 3\n",
                       sr, peak_idx, exp_arrival);
                ++failures;
            }

            /* Test 2: Median-plane bit-identical symmetry at this sample rate */
            flush_param(plugin, params, 2, 0.0); /* front (s = 0) */
            plugin->reset(plugin);
            for (uint32_t i = 0; i < multi_N; ++i) multi_in[i] = 0.0f;
            for (int b = 0; b < 20; ++b) plugin->process(plugin, &multi_proc);

            multi_in[0] = 1.0f;
            plugin->process(plugin, &multi_proc);

            if (memcmp(multi_l, multi_r, multi_N * sizeof(float)) == 0) {
                printf("SUCCESS: Bit-identical median-plane symmetry verified at %0.1f Hz.\n", sr);
            } else {
                printf("ERROR: Median plane bit symmetry broken at %0.1f Hz!\n", sr);
                ++failures;
            }
        }

        free(multi_in);
        free(multi_l);
        free(multi_r);
    }

    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    FreeLibrary(lib);

    if (failures > 0) {
        printf("CLAP test FAILED (%d check(s))\n", failures);
        return 1;
    }

    printf("CLAP test completed successfully!\n");
    return 0;
}
