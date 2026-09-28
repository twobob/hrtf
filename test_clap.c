#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <clap/clap.h>

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
        .get_extension = NULL,
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
        if (max_peak > 0.999f) {
            printf("ERROR: Loaded WAV contains railed/clipped samples (peak=%f)\n", max_peak);
            ++failures;
        } else {
            printf("SUCCESS: Loaded generated test pulse WAV file 'pulsed_pink_noise_48k.wav' (%zu samples, peak=%f, unclipped).\n",
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
        if (params->count(plugin) != 7u) {
            printf("ERROR: params count is %u, expected 7\n", params->count(plugin));
            ++failures;
        } else {
            printf("SUCCESS: params count is 7 as expected.\n");
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
            flush_param(plugin, params, 1, 8.0);    /* distance, metres */
            flush_param(plugin, params, 3, 30.0);   /* elevation, degrees */
            flush_param(plugin, params, 4, 0.40);   /* space, 0..1 */
            flush_param(plugin, params, 5, 1.0);    /* test pulse, on */
            flush_param(plugin, params, 6, 0.85);   /* test tone, 0..1 */
            flush_param(plugin, params, 7, 1.15);   /* ear scale, 0.70..1.30 */

            if (!state->save(plugin, &os)) {
                printf("ERROR: state save failed\n");
                ++failures;
            }

            flush_param(plugin, params, 2, 0.25);
            flush_param(plugin, params, 1, 1.0);
            flush_param(plugin, params, 3, 0.0);
            flush_param(plugin, params, 4, 0.0);
            flush_param(plugin, params, 5, 0.0);
            flush_param(plugin, params, 6, 0.5);
            flush_param(plugin, params, 7, 1.0);

            mem.pos = 0;
            if (!state->load(plugin, &is)) {
                printf("ERROR: state load failed\n");
                ++failures;
            }

            double d = 0.0, r = 0.0, e = 0.0, s = 0.0, p_val = 0.0, t_val = 0.0, es_val = 0.0;
            params->get_value(plugin, 1, &d);
            params->get_value(plugin, 2, &r);
            params->get_value(plugin, 3, &e);
            params->get_value(plugin, 4, &s);
            params->get_value(plugin, 5, &p_val);
            params->get_value(plugin, 6, &t_val);
            params->get_value(plugin, 7, &es_val);
            if (fabs(d - 8.0) > 1e-9) {
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
            } else {
                printf("SUCCESS: state round-trip preserved all 7 parameters.\n");
            }

            /* Verify legacy state v1 migration: resets unrepresented parameters to defaults */
            {
                MemStream v1_mem;
                memset(&v1_mem, 0, sizeof(v1_mem));
                memcpy(v1_mem.data, "HRTF", 4);
                uint32_t v1_hdr = 1;
                double v1_data[2] = { 4.5, 0.25 };
                memcpy(v1_mem.data + 4, &v1_hdr, sizeof(v1_hdr));
                memcpy(v1_mem.data + 8, v1_data, sizeof(v1_data));
                v1_mem.size = 8 + sizeof(v1_data);

                clap_istream_t in_str = { &v1_mem, mem_read };
                if (!state->load(plugin, &in_str)) {
                    printf("ERROR: failed to load legacy v1 state stream\n");
                    ++failures;
                } else {
                    double d1 = 0, r1 = 0, s1 = 0, es1 = 0;
                    params->get_value(plugin, 1, &d1);
                    params->get_value(plugin, 2, &r1);
                    params->get_value(plugin, 4, &s1);
                    params->get_value(plugin, 7, &es1);
                    if (fabs(d1 - 4.5) < 1e-9 && fabs(r1 - 0.25) < 1e-9 &&
                        fabs(s1 - 0.15) < 1e-9 && fabs(es1 - 1.0) < 1e-9) {
                        printf("SUCCESS: legacy v1 state correctly loaded and reset new parameters to defaults.\n");
                    } else {
                        printf("ERROR: legacy v1 state migration failed (d=%f, r=%f, s=%f, es=%f)\n",
                               d1, r1, s1, es1);
                        ++failures;
                    }
                }
            }

            /* Test near-field ITD delay without aliasing: d = 5 cm, ear_scale = 1.30, 90 deg right */
            {
                flush_param(plugin, params, 1, 0.05); /* 5 cm distance */
                flush_param(plugin, params, 2, 0.25); /* 90 deg right */
                flush_param(plugin, params, 3, 0.0);  /* horizontal */
                flush_param(plugin, params, 4, 0.0);  /* anechoic to isolate ITD */
                flush_param(plugin, params, 5, 0.0);  /* Test pulse OFF */
                flush_param(plugin, params, 7, 1.30); /* 130% ear scale */

                /* Clear history and wait for parameter smoothers to reach steady state (50 ms time constant) */
                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                for (int b = 0; b < 40; ++b) plugin->process(plugin, &process);

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

                /* Near-field ITD should be ~60-70 samples; if aliased to 59-sample buffer it wraps to ~2 samples */
                if (far_peak_idx >= 55 && far_peak_idx <= 75) {
                    printf("SUCCESS: Near-field ITD delay is %u samples (expected ~60-70, correctly un-aliased).\n",
                           far_peak_idx);
                } else {
                    printf("ERROR: Near-field ITD aliased! Peak arrived at sample %u instead of ~60-70\n",
                           far_peak_idx);
                    ++failures;
                }
            }

            /* Test Test Pulse "OFF" state: ensure generator shuts off cleanly with silence in */
            {
                flush_param(plugin, params, 5, 0.0); /* Disable Test Pulse */
                for (uint32_t i = 0; i < N; ++i) in_buf[i] = 0.0f;
                for (int b = 0; b < 10; ++b) plugin->process(plugin, &process);
                float silent_sum = 0.0f;
                for (uint32_t i = 0; i < N; ++i) silent_sum += fabsf(out_l[i]) + fabsf(out_r[i]);
                if (silent_sum < 1e-5f) {
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

            if (crisp_deltas > rumble_deltas * 1.5f) {
                printf("SUCCESS: Dogfooded internal test tone spectrum shift (crisp delta=%f > rumble delta=%f).\n",
                       crisp_deltas, rumble_deltas);
            } else {
                printf("ERROR: Test tone did not increase transient sharpness (rumble=%f, crisp=%f)\n",
                       rumble_deltas, crisp_deltas);
                ++failures;
            }
        }
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
