
#include "hrtf_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HRTF_PI 3.141592653589793238462643383279502884
#define HRTF_MAX_ITD_S 0.00070
#define HRTF_MAX_DELAY_SAMPLES 128

typedef struct {
    double b0,b1,b2,a1,a2;
    double z1,z2;
} Biquad;

struct HrtfCore {
    double fs;
    size_t max_block;
    double distance_m;
    double phase;
    double elevation_deg;
    double space;
    double phase_smooth;
    double distance_smooth;
    double elevation_smooth;
    double space_smooth;

    float *delay_l;
    float *delay_r;
    size_t delay_size;
    size_t delay_pos;

    float *early_buf;
    size_t early_size;
    size_t early_pos;
    double early_lpf_l;
    double early_lpf_r;

    Biquad l[5];
    Biquad r[5];

    double last_phase;
    double last_distance;
};

static double clampd(double x, double lo, double hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static double wrap01(double x)
{
    x -= floor(x);
    return x < 0.0 ? x + 1.0 : x;
}

static void biquad_reset(Biquad *q)
{
    q->z1 = q->z2 = 0.0;
}

static void biquad_peaking(Biquad *q, double fs, double f0, double qv, double gain_db)
{
    const double A = pow(10.0, gain_db / 40.0);
    const double w = 2.0 * HRTF_PI * f0 / fs;
    const double c = cos(w);
    const double s = sin(w);
    const double alpha = s / (2.0 * qv);

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * c;
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * c;
    const double a2 = 1.0 - alpha / A;

    q->b0 = b0/a0; q->b1 = b1/a0; q->b2 = b2/a0;
    q->a1 = a1/a0; q->a2 = a2/a0;
}

static void biquad_highshelf(Biquad *q, double fs, double f0, double gain_db)
{
    const double A = pow(10.0, gain_db / 40.0);
    const double w = 2.0 * HRTF_PI * f0 / fs;
    const double c = cos(w);
    const double s = sin(w);
    const double alpha = s / 2.0;
    const double beta = 2.0 * sqrt(A) * alpha;

    const double b0 = A*((A+1)+(A-1)*c+beta);
    const double b1 = -2*A*((A-1)+(A+1)*c);
    const double b2 = A*((A+1)+(A-1)*c-beta);
    const double a0 = (A+1)-(A-1)*c+beta;
    const double a1 = 2*((A-1)-(A+1)*c);
    const double a2 = (A+1)-(A-1)*c-beta;

    q->b0=b0/a0; q->b1=b1/a0; q->b2=b2/a0;
    q->a1=a1/a0; q->a2=a2/a0;
}

static void biquad_lowshelf(Biquad *q, double fs, double f0, double gain_db)
{
    const double A = pow(10.0, gain_db / 40.0);
    const double w = 2.0 * HRTF_PI * f0 / fs;
    const double c = cos(w);
    const double s = sin(w);
    const double alpha = s / 2.0;
    const double beta = 2.0 * sqrt(A) * alpha;

    const double b0 =    A * ( (A+1) - (A-1)*c + beta );
    const double b1 =  2*A * ( (A-1) - (A+1)*c );
    const double b2 =    A * ( (A+1) - (A-1)*c - beta );
    const double a0 =          (A+1) + (A-1)*c + beta;
    const double a1 =   -2 * ( (A-1) + (A+1)*c );
    const double a2 =          (A+1) + (A-1)*c - beta;

    q->b0 = b0/a0; q->b1 = b1/a0; q->b2 = b2/a0;
    q->a1 = a1/a0; q->a2 = a2/a0;
}

static double biquad_process(Biquad *q, double x)
{
    /* Transposed direct form II. */
    const double y = q->b0*x + q->z1;
    q->z1 = q->b1*x - q->a1*y + q->z2;
    q->z2 = q->b2*x - q->a2*y;
    return y;
}

static double interp_delay(const float *buf, size_t size, double write_pos, double delay)
{
    double p = write_pos - delay;
    while (p < 0.0) p += (double)size;
    while (p >= (double)size) p -= (double)size;

    const size_t i0 = (size_t)p;
    const size_t i1 = (i0 + 1) % size;
    const double f = p - (double)i0;
    return (double)buf[i0] * (1.0-f) + (double)buf[i1] * f;
}

static void design_filters(HrtfCore *h, double phase, double elevation_deg, double distance,
                           Biquad *l, Biquad *r)
{
    /* Spherical coordinate conventions:
       phase [0, 1): 0.00 = front, 0.25 = right, 0.50 = rear, 0.75 = left.
       elevation [-90, +90] deg: -90 = below, 0 = horizontal, +90 = overhead. */

    const double theta = phase * 2.0 * HRTF_PI;
    const double phi = elevation_deg * (HRTF_PI / 180.0);
    const double cos_phi = cos(phi);
    const double sin_phi = sin(phi);

    /* Direction cosines */
    const double s = cos_phi * sin(theta); /* Lateral (-1 = left, +1 = right) */
    const double c = cos_phi * cos(theta); /* Front/Rear (+1 = front, -1 = rear) */
    const double v = sin_phi;              /* Vertical (+1 = overhead, -1 = below) */

    /* Front/rear interpolation. front=1 at 0°, rear=1 at 180°. */
    const double front = 0.5 * (1.0 + c);
    const double rear  = 1.0 - front;

    /* Side energy: zero at front/rear/overhead, one at lateral 90°/270°. */
    const double side = fabs(s);

    /* Near-field compensation: subtle proximity cue below 1 metre without gain runaway. */
    const double near = (distance < 1.0) ? clampd(1.0 - distance, 0.0, 1.0) : 0.0;

    /* Graph-derived front presence around 3-5 kHz, reduced overhead/below */
    const double presence = 9.0 * front + 1.0 * rear + 1.5 * near - 3.0 * fabs(v);

    /* Dynamic elevation concha notch:
       Median plane notch shifts from ~4.5 kHz (below) up to ~9.5 kHz (overhead). */
    const double f_notch = clampd(6500.0 + 3000.0 * v, 4200.0, 11000.0);
    const double notch = -4.0 * rear - 3.5 * clampd(-v, 0.0, 1.0);

    /* High-frequency air falls towards rear, and undergoes cranial shadow from overhead. */
    const double overhead_shadow = clampd(v, 0.0, 1.0);
    const double air = 2.0 * front - 8.0 * rear - 1.5 * side + 1.0 * near - 3.0 * overhead_shadow;

    /* Side-specific head shadow / pinna asymmetry: the far ear loses more high frequency.
       When s > 0 (sound on right), left ear is far. When s < 0, right ear is far. */
    const double left_far  = clampd(s, 0.0, 1.0);
    const double right_far = clampd(-s, 0.0, 1.0);

    biquad_peaking(&l[0], h->fs, 3900.0, 0.85, presence);
    biquad_peaking(&r[0], h->fs, 3900.0, 0.85, presence);

    biquad_peaking(&l[1], h->fs, f_notch, 3.0, notch);
    biquad_peaking(&r[1], h->fs, f_notch, 3.0, notch);

    biquad_highshelf(&l[2], h->fs, 8500.0, air - 3.5*left_far);
    biquad_highshelf(&r[2], h->fs, 8500.0, air - 3.5*right_far);

    /* A broad 2.2 kHz side cue keeps lateral positions from sounding like
       simple left/right gain panning. */
    biquad_peaking(&l[3], h->fs, 2200.0, 0.9, -2.0*left_far);
    biquad_peaking(&r[3], h->fs, 2200.0, 0.9, -2.0*right_far);

    /* Near-field low-frequency ILD divergence (Distance Variation Function / DVF).
       In the near field (d < 1.0 m), spherical wavefront curvature produces significant
       low-frequency ILD (up to +10 dB boost on near ear, and cut on far ear). */
    double nf_gain_l = 0.0;
    double nf_gain_r = 0.0;
    if (distance < 1.0) {
        const double d_norm = clampd(distance, 0.05, 1.0);
        const double prox = (1.0 - d_norm); /* 0 at 1m, 0.95 at 0.05m */
        const double nf_ild = 10.0 * prox * fabs(s); /* up to 9.5 dB difference */
        if (s > 0.0) {
            nf_gain_r = +0.5 * nf_ild;
            nf_gain_l = -0.5 * nf_ild;
        } else if (s < 0.0) {
            nf_gain_l = +0.5 * nf_ild;
            nf_gain_r = -0.5 * nf_ild;
        }
    }
    biquad_lowshelf(&l[4], h->fs, 350.0, nf_gain_l);
    biquad_lowshelf(&r[4], h->fs, 350.0, nf_gain_r);
}

HrtfCore *hrtf_create(double sample_rate, size_t max_block)
{
    if (!(sample_rate > 1000.0) || max_block == 0) return NULL;

    HrtfCore *h = (HrtfCore *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->fs = sample_rate;
    h->max_block = max_block;
    h->distance_m = h->distance_smooth = 2.0;
    h->phase = h->phase_smooth = 0.0;
    h->elevation_deg = h->elevation_smooth = 0.0;
    h->space = h->space_smooth = 0.15; /* 15% default room externalisation */

    size_t needed = (size_t)ceil(sample_rate * HRTF_MAX_ITD_S) + 4;
    if (needed < 16) needed = 16;
    if (needed > HRTF_MAX_DELAY_SAMPLES * 4) needed = HRTF_MAX_DELAY_SAMPLES * 4;
    h->delay_size = needed;

    h->delay_l = (float *)calloc(h->delay_size, sizeof(float));
    h->delay_r = (float *)calloc(h->delay_size, sizeof(float));

    h->early_size = (size_t)ceil(sample_rate * 0.050) + 64; /* 50 ms max room reflection */
    h->early_buf = (float *)calloc(h->early_size, sizeof(float));

    if (!h->delay_l || !h->delay_r || !h->early_buf) {
        hrtf_destroy(h);
        return NULL;
    }

    hrtf_reset(h);
    return h;
}

void hrtf_destroy(HrtfCore *h)
{
    if (!h) return;
    free(h->delay_l);
    free(h->delay_r);
    free(h->early_buf);
    free(h);
}

void hrtf_reset(HrtfCore *h)
{
    if (!h) return;

    memset(h->delay_l, 0, h->delay_size * sizeof(float));
    memset(h->delay_r, 0, h->delay_size * sizeof(float));
    h->delay_pos = 0;

    if (h->early_buf) {
        memset(h->early_buf, 0, h->early_size * sizeof(float));
    }
    h->early_pos = 0;
    h->early_lpf_l = 0.0;
    h->early_lpf_r = 0.0;

    for (size_t i=0; i<5; ++i) {
        biquad_reset(&h->l[i]);
        biquad_reset(&h->r[i]);
    }

    h->phase_smooth = wrap01(h->phase);
    h->distance_smooth = clampd(h->distance_m, 0.05, 20.0);
    h->elevation_smooth = clampd(h->elevation_deg, -90.0, 90.0);
    h->space_smooth = clampd(h->space, 0.0, 1.0);
    h->last_phase = h->phase_smooth;
    h->last_distance = h->distance_smooth;
}

void hrtf_set_distance(HrtfCore *h, double distance_m)
{
    /* A non-finite value (hostile automation, corrupt preset) would spread
       through the smoothers and silence the output permanently. */
    if (!h || !isfinite(distance_m)) return;
    h->distance_m = clampd(distance_m, 0.05, 20.0);
}

void hrtf_set_rotation_phase(HrtfCore *h, double phase)
{
    if (!h || !isfinite(phase)) return;
    h->phase = wrap01(phase);
}

void hrtf_set_elevation_deg(HrtfCore *h, double elevation_deg)
{
    if (!h || !isfinite(elevation_deg)) return;
    h->elevation_deg = clampd(elevation_deg, -90.0, 90.0);
}

void hrtf_set_space(HrtfCore *h, double space_01)
{
    if (!h || !isfinite(space_01)) return;
    h->space = clampd(space_01, 0.0, 1.0);
}

static inline float soft_limit(float x)
{
    const float T = 0.89125f; /* -1.0 dBFS linear threshold */
    if (x > T) {
        return T + (1.0f - T) * tanhf((x - T) / (1.0f - T));
    } else if (x < -T) {
        return -T - (1.0f - T) * tanhf((-x - T) / (1.0f - T));
    }
    return x;
}

void hrtf_process(HrtfCore *h, const float *mono, float **stereo, size_t n)
{
    if (!h || !mono || !stereo || !stereo[0] || !stereo[1]) return;

    /* Belt and braces: once a non-finite value reaches the filter state the
       output never recovers, so re-seed the smoothers if that ever happens. */
    if (!isfinite(h->phase_smooth)) h->phase_smooth = 0.0;
    if (!isfinite(h->distance_smooth)) h->distance_smooth = 2.0;
    if (!isfinite(h->elevation_smooth)) h->elevation_smooth = 0.0;
    if (!isfinite(h->space_smooth)) h->space_smooth = 0.15;

    const double param_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms */
    const double distance_slew = exp(-1.0 / (0.050 * h->fs)); /* 50 ms */
    const double elev_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms */
    const double space_slew = exp(-1.0 / (0.030 * h->fs)); /* 30 ms */

    for (size_t i=0; i<n; ++i) {
        double dp = h->phase - h->phase_smooth;
        if (dp > 0.5) dp -= 1.0;
        if (dp < -0.5) dp += 1.0;
        h->phase_smooth = wrap01(h->phase_smooth + (1.0-param_slew) * dp);

        h->distance_smooth += (1.0-distance_slew) *
                              (h->distance_m - h->distance_smooth);

        h->elevation_smooth += (1.0-elev_slew) *
                               (h->elevation_deg - h->elevation_smooth);

        h->space_smooth += (1.0-space_slew) *
                           (h->space - h->space_smooth);

        const double theta = h->phase_smooth * 2.0 * HRTF_PI;
        const double phi = h->elevation_smooth * (HRTF_PI / 180.0);
        const double cos_phi = cos(phi);
        const double s = cos_phi * sin(theta); /* Lateral component */

        /* Spherical head shadow / Interaural Level Difference (ILD).
           At 90 degrees lateral (s = +1), the near ear (right) is 0 dB,
           and the far ear (left) is shadowed by -8 dB (far_gain = 0.3981).
           Front, rear, and overhead remain symmetric (s = 0 -> gl = gr = 1.0). */
        const double far_gain = 0.3981071706; /* -8 dB */
        double gl = 1.0;
        double gr = 1.0;
        if (s > 0.0) {
            /* Source on right: right ear is near, left ear is shadowed */
            gl = 1.0 - s * (1.0 - far_gain);
        } else if (s < 0.0) {
            /* Source on left: left ear is near, right ear is shadowed */
            gr = 1.0 - (-s) * (1.0 - far_gain);
        }

        const double norm = sqrt(0.5 * (gl*gl + gr*gr));
        if (norm > 0.0) {
            gl /= norm;
            gr /= norm;
        }

        /* Distance attenuation referenced to 1 metre.
           Below 1 metre: limited to 1.0 (0 dB maximum distance gain)
           so that moving closer than 1m never overdrives or clips.
           Above 1 metre: follows standard 1/d free-field inverse-distance law. */
        const double d = h->distance_smooth;
        double distance_gain = (d >= 1.0) ? (1.0 / d) : 1.0;
        distance_gain = clampd(distance_gain, 0.05, 1.0);

        /* Near-field ITD correction: spherical wavefront curvature increases
           effective acoustic path length to far ear at distances < 1m. */
        const double nf_itd_scale = (d < 1.0) ? (1.0 + 0.00765 / (2.0 * d * d + 0.00765)) : 1.0;

        /* ITD: max ~0.70 ms scaled by lateral projection and near-field factor. */
        const double itd_s = HRTF_MAX_ITD_S * s * nf_itd_scale;
        const double itd_samples = itd_s * h->fs;
        double dl = 0.0, dr = 0.0;
        if (itd_samples >= 0.0) dl = itd_samples;  /* source on right -> delay left ear */
        else dr = -itd_samples;                    /* source on left -> delay right ear */

        const float x = mono[i];
        h->delay_l[h->delay_pos] = x;
        h->delay_r[h->delay_pos] = x;

        const double xl = interp_delay(h->delay_l, h->delay_size,
                                       (double)h->delay_pos, dl);
        const double xr = interp_delay(h->delay_r, h->delay_size,
                                       (double)h->delay_pos, dr);

        /* Re-design coefficients at a modest control rate. The parameter
           smoother runs at audio rate; 16-sample coefficient updates keep
           zippering negligible while avoiding per-sample trig/coefficient work. */
        if ((i & 15u) == 0u) {
            design_filters(h, h->phase_smooth, h->elevation_smooth, h->distance_smooth, h->l, h->r);
        }

        double yl = xl;
        double yr = xr;
        for (int k=0; k<5; ++k) {
            yl = biquad_process(&h->l[k], yl);
            yr = biquad_process(&h->r[k], yr);
        }

        /* Early reflection engine for room externalisation:
           Feeds a delay line with boundary reflection taps (floor, ceiling, walls). */
        if (h->early_buf) {
            h->early_buf[h->early_pos] = x;
            const size_t sz = h->early_size;
            const size_t pos = h->early_pos;

            const size_t t1 = (pos + sz - (size_t)(0.0048 * h->fs)) % sz; /* Floor */
            const size_t t2 = (pos + sz - (size_t)(0.0081 * h->fs)) % sz; /* Ceiling */
            const size_t t3 = (pos + sz - (size_t)(0.0135 * h->fs)) % sz; /* Left wall */
            const size_t t4 = (pos + sz - (size_t)(0.0162 * h->fs)) % sz; /* Right wall */
            const size_t t5 = (pos + sz - (size_t)(0.0234 * h->fs)) % sz; /* Back wall */

            const float r1 = h->early_buf[t1] * 0.25f;
            const float r2 = h->early_buf[t2] * 0.22f;
            const float r3 = h->early_buf[t3] * 0.20f;
            const float r4 = h->early_buf[t4] * 0.20f;
            const float r5 = h->early_buf[t5] * 0.16f;

            const double raw_l = r1 + r2 + r3 + 0.35 * r4 + r5;
            const double raw_r = r1 + r2 + 0.35 * r3 + r4 + r5;

            /* Gentle 1-pole wall absorption filter */
            h->early_lpf_l = 0.65 * raw_l + 0.35 * h->early_lpf_l;
            h->early_lpf_r = 0.65 * raw_r + 0.35 * h->early_lpf_r;

            h->early_pos++;
            if (h->early_pos >= h->early_size) h->early_pos = 0;
        }

        /* Master headroom scale (0.7071 = -3.0 dB) to ensure pinna resonance peaks
           do not exceed 0 dBFS at 1m distance for a full-scale input. */
        const double master_headroom = 0.7071067811865475;
        const double space_gain = h->space_smooth * 0.40;

        float out0 = (float)(yl * gl * distance_gain * master_headroom + h->early_lpf_l * space_gain * distance_gain);
        float out1 = (float)(yr * gr * distance_gain * master_headroom + h->early_lpf_r * space_gain * distance_gain);

        /* Soft-knee saturation ceiling: guarantees peak amplitude never exceeds 0 dBFS */
        stereo[0][i] = soft_limit(out0);
        stereo[1][i] = soft_limit(out1);

        h->delay_pos++;
        if (h->delay_pos >= h->delay_size) h->delay_pos = 0;
    }
}

/* -------------------------------------------------------------------------
   Test Signal Generator: 200 ms 1/f Pink Noise Bursts on Each Beat
   ------------------------------------------------------------------------- */

void hrtf_test_gen_init(HrtfTestGen *gen, double sample_rate)
{
    if (!gen) return;
    memset(gen, 0, sizeof(*gen));
    gen->sample_rate = sample_rate > 1000.0 ? sample_rate : 48000.0;
    gen->prng_state = 0x5A17F00Du;
}

void hrtf_test_gen_process(HrtfTestGen *gen, float *out_mono, size_t n, double bpm, double beat_pos, int is_playing)
{
    if (!gen || !out_mono || n == 0) return;

    if (bpm < 20.0 || bpm > 400.0 || !isfinite(bpm))
        bpm = 120.0;

    const double fs = gen->sample_rate;
    const double sec_per_beat = 60.0 / bpm;
    const double samples_per_beat = fs * sec_per_beat;

    /* Pulse duration: 200 ms calibrated window, capped at 60% of beat duration */
    double pulse_dur = 0.200;
    if (pulse_dur > 0.60 * sec_per_beat)
        pulse_dur = 0.60 * sec_per_beat;

    const double t_att = 0.005; /* 5 ms attack */
    const double t_rel = 0.005; /* 5 ms release */
    const double beats_per_sample = bpm / (60.0 * fs);

    for (size_t i = 0; i < n; ++i) {
        double t_in_beat = 0.0;

        if (is_playing) {
            double current_beat = beat_pos + (double)i * beats_per_sample;
            double phase = current_beat - floor(current_beat);
            t_in_beat = phase * sec_per_beat;
            gen->free_sample_counter = phase * samples_per_beat;
        } else {
            t_in_beat = gen->free_sample_counter / fs;
            gen->free_sample_counter += 1.0;
            if (gen->free_sample_counter >= samples_per_beat)
                gen->free_sample_counter -= samples_per_beat;
        }

        /* Calculate raised-cosine (Hann) envelope */
        float envelope = 0.0f;
        if (t_in_beat >= 0.0 && t_in_beat < pulse_dur) {
            if (t_in_beat < t_att) {
                envelope = 0.5f * (1.0f - (float)cos(HRTF_PI * t_in_beat / t_att));
            } else if (t_in_beat > pulse_dur - t_rel) {
                envelope = 0.5f * (1.0f + (float)cos(HRTF_PI * (t_in_beat - (pulse_dur - t_rel)) / t_rel));
            } else {
                envelope = 1.0f;
            }
        }

        /* 32-bit xorshift PRNG */
        gen->prng_state ^= gen->prng_state << 13;
        gen->prng_state ^= gen->prng_state >> 17;
        gen->prng_state ^= gen->prng_state << 5;
        float white = (float)(int32_t)gen->prng_state * (1.0f / 2147483648.0f);

        /* Paul Kellet's refined 7-pole 1/f pink noise filter */
        gen->b0 = 0.99886f * gen->b0 + white * 0.0555179f;
        gen->b1 = 0.99332f * gen->b1 + white * 0.0750759f;
        gen->b2 = 0.96900f * gen->b2 + white * 0.1538520f;
        gen->b3 = 0.86650f * gen->b3 + white * 0.3104856f;
        gen->b4 = 0.55000f * gen->b4 + white * 0.5329522f;
        gen->b5 = -0.7616f * gen->b5 - white * 0.0168980f;
        float pink = gen->b0 + gen->b1 + gen->b2 + gen->b3 + gen->b4 + gen->b5 + gen->b6 + white * 0.5362f;
        gen->b6 = white * 0.115926f;

        /* Calibrated output level (-14 dBFS nominal burst RMS) */
        out_mono[i] = pink * 0.18f * envelope;
    }
}

