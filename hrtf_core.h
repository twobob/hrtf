
#ifndef HRTF_CORE_H
#define HRTF_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HrtfCore HrtfCore;

#define HRTF_STATE_VERSION 4

/* distance_m: metres, clamped to 0.05..20.
   rotation_phase: 0..1, periodic; 0=front, .25=right, .5=back, .75=left.
   elevation_deg: degrees, -90..+90; -90=below, 0=horizontal, +90=overhead.
   space_01: room externalisation, 0..1; 0=anechoic dry, 1=maximum room reflections. */
HrtfCore *hrtf_create(double sample_rate);
void hrtf_destroy(HrtfCore *h);
void hrtf_reset(HrtfCore *h);
void hrtf_set_distance(HrtfCore *h, double distance_m);
void hrtf_set_rotation_phase(HrtfCore *h, double phase);
void hrtf_set_elevation_deg(HrtfCore *h, double elevation_deg);
void hrtf_set_space(HrtfCore *h, double space_01);
void hrtf_set_ear_scale(HrtfCore *h, double scale);
void hrtf_process(HrtfCore *h, const float *mono, float **stereo, size_t n);

/* Test generator signal modes */
enum {
    HRTF_TEST_MODE_NOISE = 0,
    HRTF_TEST_MODE_SINE  = 1,
    HRTF_TEST_MODE_CLICK = 2
};

/* Psychoacoustically calibrated pulse and tone test generator */
typedef struct HrtfTestGen {
    double sample_rate;
    uint32_t prng_state;
    float b0, b1, b2, b3, b4, b5, b6;
    float rumble_lpf;
    double free_sample_counter;
    double tone;            /* 0.0 = low rumble, 0.5 = pink noise, 1.0 = crisp transient */
    int mode;               /* HRTF_TEST_MODE_NOISE, SINE, CLICK */
    double freq_hz;         /* Sine test tone frequency */
    double pulse_dur_s;     /* Pulse duration in seconds */
    double sine_phase;
} HrtfTestGen;

/* Initialise the test generator state */
void hrtf_test_gen_init(HrtfTestGen *gen, double sample_rate);

/* Adjust test generator parameters */
void hrtf_test_gen_set_tone(HrtfTestGen *gen, double tone);
void hrtf_test_gen_set_mode(HrtfTestGen *gen, int mode);
void hrtf_test_gen_set_frequency(HrtfTestGen *gen, double freq_hz);
void hrtf_test_gen_set_duration(HrtfTestGen *gen, double dur_s);

/* Synthesise pulses aligned to beats */
void hrtf_test_gen_process(HrtfTestGen *gen, float *out_mono, size_t n, double bpm, double beat_pos, int is_playing);

#ifdef __cplusplus
}
#endif
#endif
