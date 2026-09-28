/*
 * generate_test_pulse.c
 *
 * Standalone pure C command-line tool to synthesise calibrated pulsed pink noise WAV signals.
 * Directly links against hrtf_core.c to guarantee acoustic parity with the plugin's internal generator.
 *
 * Standard format: 48 kHz, 24-bit PCM mono, loopable 8-beat pattern at 120 BPM (4.0 seconds).
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "../hrtf_core.h"

#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];        /* "RIFF" */
    uint32_t riff_size;         /* Overall file size - 8 bytes */
    char     wave_id[4];        /* "WAVE" */
    char     fmt_id[4];         /* "fmt " */
    uint32_t fmt_size;          /* 16 bytes for PCM */
    uint16_t audio_format;      /* 1 = Linear PCM */
    uint16_t num_channels;      /* 1 = Mono */
    uint32_t sample_rate;       /* e.g. 48000 Hz */
    uint32_t byte_rate;         /* sample_rate * num_channels * (bits_per_sample / 8) */
    uint16_t block_align;       /* num_channels * (bits_per_sample / 8) */
    uint16_t bits_per_sample;   /* 24 bits */
    char     data_id[4];        /* "data" */
    uint32_t data_size;         /* num_samples * num_channels * (bits_per_sample / 8) */
} WavHeader;
#pragma pack(pop)

static int write_wav_file(const char *filename, const float *samples, size_t num_samples, uint32_t fs)
{
    /* Ensure parent directory exists if specified in path */
    const char *slash = strrchr(filename, '/');
    if (!slash) slash = strrchr(filename, '\\');
    if (slash) {
        char dir[256];
        size_t len = (size_t)(slash - filename);
        if (len < sizeof(dir)) {
            memcpy(dir, filename, len);
            dir[len] = '\0';
#ifdef _WIN32
            _mkdir(dir);
#else
            mkdir(dir, 0755);
#endif
        }
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: Unable to open file for writing: %s\n", filename);
        return 0;
    }

    uint16_t bytes_per_sample = 3; /* 24-bit PCM */
    uint32_t data_bytes = (uint32_t)(num_samples * bytes_per_sample);

    WavHeader hdr;
    memcpy(hdr.riff_id, "RIFF", 4);
    hdr.riff_size = sizeof(WavHeader) - 8 + data_bytes;
    memcpy(hdr.wave_id, "WAVE", 4);
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = 1;
    hdr.sample_rate = fs;
    hdr.byte_rate = fs * bytes_per_sample;
    hdr.block_align = bytes_per_sample;
    hdr.bits_per_sample = 24;
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_bytes;

    if (fwrite(&hdr, sizeof(WavHeader), 1, f) != 1) {
        fprintf(stderr, "ERROR: Failed to write WAV header to: %s\n", filename);
        fclose(f);
        return 0;
    }

    /* Check for peak amplitude to scale cleanly if necessary (preventing any hard clipping) */
    double peak = 0.0;
    for (size_t i = 0; i < num_samples; ++i) {
        double a = fabs((double)samples[i]);
        if (a > peak) peak = a;
    }
    double norm_factor = 1.0;
    if (peak > 0.999) {
        norm_factor = 0.95 / peak;
    }

    const double scale = 8388607.0; /* 2^23 - 1 */

    for (size_t i = 0; i < num_samples; ++i) {
        double s = (double)samples[i] * norm_factor;
        if (s > 0.999999) s = 0.999999;
        if (s < -0.999999) s = -0.999999;

        int32_t v = (int32_t)floor(s * scale + 0.5);
        if (v > 8388607) v = 8388607;
        if (v < -8388608) v = -8388608;

        uint8_t bytes[3];
        bytes[0] = (uint8_t)(v & 0xFF);
        bytes[1] = (uint8_t)((v >> 8) & 0xFF);
        bytes[2] = (uint8_t)((v >> 16) & 0xFF);

        if (fwrite(bytes, 1, 3, f) != 3) {
            fprintf(stderr, "ERROR: Write failure at sample %zu\n", i);
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    printf("Successfully wrote: %s (%zu samples, %.2f s, 24-bit @ %u Hz)\n",
           filename, num_samples, (double)num_samples / fs, fs);
    return 1;
}

static void print_usage(const char *prog_name)
{
    printf("Rotating HRTF Test Pulse Generator (Pure C CLI)\n");
    printf("Synthesises calibrated test signals for 3D HRTF spatialisation benchmarking.\n\n");
    printf("Usage: %s [options]\n\n", prog_name);
    printf("Options:\n");
    printf("  -o, --output <path>       Output WAV file path (default writes to standard test files)\n");
    printf("  -t, --tone <0.0..1.0>     Tone morphing: 0.0=low rumble, 0.5=pink noise, 1.0=crisp transient (default: 0.5)\n");
    printf("  -m, --mode <mode>         Signal mode: noise, sine, click (default: noise)\n");
    printf("  -f, --freq <Hz>           Sine test frequency in Hz (default: 1000.0)\n");
    printf("  -b, --bpm <BPM>           Tempo in beats per minute (default: 120.0)\n");
    printf("  -n, --beats <count>       Total number of beats to synthesise (default: 8)\n");
    printf("  -d, --dur <ms>            Pulse duration in milliseconds (default: 200.0)\n");
    printf("  -s, --samplerate <Hz>     Audio sample rate in Hz (default: 48000)\n");
    printf("  -h, --help                Display this help message and exit\n");
}

int main(int argc, char **argv)
{
    const char *output_file = NULL;
    double tone = 0.5;
    int mode = HRTF_TEST_MODE_NOISE;
    double freq_hz = 1000.0;
    double bpm = 120.0;
    int total_beats = 8;
    double dur_ms = 200.0;
    uint32_t fs = 48000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output_file = argv[++i];
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tone") == 0) && i + 1 < argc) {
            tone = atof(argv[++i]);
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) && i + 1 < argc) {
            const char *m = argv[++i];
            if (_stricmp(m, "sine") == 0) mode = HRTF_TEST_MODE_SINE;
            else if (_stricmp(m, "click") == 0) mode = HRTF_TEST_MODE_CLICK;
            else mode = HRTF_TEST_MODE_NOISE;
        } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--freq") == 0) && i + 1 < argc) {
            freq_hz = atof(argv[++i]);
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bpm") == 0) && i + 1 < argc) {
            bpm = atof(argv[++i]);
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--beats") == 0) && i + 1 < argc) {
            total_beats = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dur") == 0) && i + 1 < argc) {
            dur_ms = atof(argv[++i]);
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--samplerate") == 0) && i + 1 < argc) {
            fs = (uint32_t)atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (fs < 8000 || fs > 192000) fs = 48000;
    if (bpm < 20.0 || bpm > 400.0) bpm = 120.0;
    if (total_beats < 1 || total_beats > 128) total_beats = 8;
    if (dur_ms < 1.0 || dur_ms > 2000.0) dur_ms = 200.0;

    const double total_seconds = (double)total_beats * (60.0 / bpm);
    const size_t total_samples = (size_t)(total_seconds * fs);

    float *buffer = (float *)malloc(total_samples * sizeof(float));
    if (!buffer) {
        fprintf(stderr, "ERROR: Out of memory allocating sample buffer\n");
        return 1;
    }

    /* Initialise test generator from hrtf_core */
    HrtfTestGen gen;
    hrtf_test_gen_init(&gen, fs);
    hrtf_test_gen_set_tone(&gen, tone);
    hrtf_test_gen_set_mode(&gen, mode);
    hrtf_test_gen_set_frequency(&gen, freq_hz);
    hrtf_test_gen_set_duration(&gen, dur_ms / 1000.0);

    /* Synthesise audio in blocks */
    const size_t block_size = 256;
    size_t cursor = 0;
    const double beats_per_sample = bpm / (60.0 * (double)fs);

    while (cursor < total_samples) {
        size_t chunk = block_size;
        if (cursor + chunk > total_samples) {
            chunk = total_samples - cursor;
        }

        double beat_pos = (double)cursor * beats_per_sample;
        hrtf_test_gen_process(&gen, buffer + cursor, chunk, bpm, beat_pos, 1);
        cursor += chunk;
    }

    int ok = 1;
    if (output_file) {
        ok = write_wav_file(output_file, buffer, total_samples, fs);
    } else {
        int ok1 = write_wav_file("pulsed_pink_noise_48k.wav", buffer, total_samples, fs);
        int ok2 = write_wav_file("test_signals/pulsed_pink_noise_48k.wav", buffer, total_samples, fs);
        ok = ok1 && ok2;
    }

    free(buffer);
    return ok ? 0 : 1;
}
