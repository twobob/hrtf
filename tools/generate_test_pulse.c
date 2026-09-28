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

    const double scale = 8388607.0; /* 2^23 - 1 */

    for (size_t i = 0; i < num_samples; ++i) {
        double s = samples[i];
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const uint32_t fs = 48000;
    const double bpm = 120.0;
    const int total_beats = 8; /* 2 bars of 4/4 */
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

    int ok1 = write_wav_file("pulsed_pink_noise_48k.wav", buffer, total_samples, fs);
    int ok2 = write_wav_file("test_signals/pulsed_pink_noise_48k.wav", buffer, total_samples, fs);

    free(buffer);

    return (ok1 && ok2) ? 0 : 1;
}
