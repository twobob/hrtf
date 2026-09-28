
#ifndef HRTF_CORE_H
#define HRTF_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HrtfCore HrtfCore;

/* distance_m: metres, clamped to 0.05..20.
   rotation_phase: 0..1, periodic; 0=front, .25=right, .5=back, .75=left.
   elevation_deg: degrees, -90..+90; -90=below, 0=horizontal, +90=overhead.
   space_01: room externalisation, 0..1; 0=anechoic dry, 1=maximum room reflections. */
HrtfCore *hrtf_create(double sample_rate, size_t max_block);
void hrtf_destroy(HrtfCore *h);
void hrtf_reset(HrtfCore *h);
void hrtf_set_distance(HrtfCore *h, double distance_m);
void hrtf_set_rotation_phase(HrtfCore *h, double phase);
void hrtf_set_elevation_deg(HrtfCore *h, double elevation_deg);
void hrtf_set_space(HrtfCore *h, double space_01);
void hrtf_process(HrtfCore *h, const float *mono, float **stereo, size_t n);

/* Psychoacoustically calibrated pulsed pink noise test generator */
typedef struct HrtfTestGen {
    double sample_rate;
    unsigned int prng_state;
    float b0, b1, b2, b3, b4, b5, b6;
    double free_sample_counter;
} HrtfTestGen;

/* Initialise the test pulse generator state */
void hrtf_test_gen_init(HrtfTestGen *gen, double sample_rate);

/* Synthesise 200 ms pink noise bursts on each beat */
void hrtf_test_gen_process(HrtfTestGen *gen, float *out_mono, size_t n, double bpm, double beat_pos, int is_playing);

#ifdef __cplusplus
}
#endif
#endif
