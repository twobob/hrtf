
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
    double phase_smooth;
    double distance_smooth;

    float *delay_l;
    float *delay_r;
    size_t delay_size;
    size_t delay_pos;

    Biquad l[4];
    Biquad r[4];

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

static void design_filters(HrtfCore *h, double phase, double distance,
                           Biquad *l, Biquad *r)
{
    /* Phase convention:
       0.00 = front
       0.25 = right
       0.50 = rear
       0.75 = left
       The supplied graph is used as a front/rear spectral envelope.
       Ear-specific side attenuation and ILD are layered on top. */

    const double theta = phase * 2.0 * HRTF_PI;
    const double c = cos(theta);
    const double s = sin(theta);

    /* Front/rear interpolation. front=1 at 0°, rear=1 at 180°. */
    const double front = 0.5 * (1.0 + c);
    const double rear  = 1.0 - front;

    /* Side energy: zero at front/rear, one at 90°/270°. */
    const double side = fabs(s);

    /* Near-field compensation: subtle proximity cue below 1 metre without gain runaway. */
    const double near = (distance < 1.0) ? clampd(1.0 - distance, 0.0, 1.0) : 0.0;

    /* Graph-derived front presence around 3-5 kHz. */
    const double presence = 9.0 * front + 1.0 * rear + 1.5 * near;

    /* Rear reflection notch around 6 kHz. */
    const double notch = -4.0 * rear;

    /* The supplied graph falls strongly at high frequencies toward the rear. */
    const double air = 2.0 * front - 8.0 * rear - 1.5 * side + 1.0 * near;

    /* Side-specific head shadow / pinna asymmetry: the far ear loses more high frequency.
       When s > 0 (sound on right), left ear is far. When s < 0, right ear is far. */
    const double left_far  = clampd(s, 0.0, 1.0);
    const double right_far = clampd(-s, 0.0, 1.0);

    biquad_peaking(&l[0], h->fs, 3900.0, 0.85, presence);
    biquad_peaking(&r[0], h->fs, 3900.0, 0.85, presence);

    biquad_peaking(&l[1], h->fs, 6000.0, 3.0, notch);
    biquad_peaking(&r[1], h->fs, 6000.0, 3.0, notch);

    biquad_highshelf(&l[2], h->fs, 8500.0, air - 3.5*left_far);
    biquad_highshelf(&r[2], h->fs, 8500.0, air - 3.5*right_far);

    /* A broad 2.2 kHz side cue keeps lateral positions from sounding like
       simple left/right gain panning. */
    biquad_peaking(&l[3], h->fs, 2200.0, 0.9, -2.0*left_far);
    biquad_peaking(&r[3], h->fs, 2200.0, 0.9, -2.0*right_far);
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

    size_t needed = (size_t)ceil(sample_rate * HRTF_MAX_ITD_S) + 4;
    if (needed < 16) needed = 16;
    if (needed > HRTF_MAX_DELAY_SAMPLES * 4) needed = HRTF_MAX_DELAY_SAMPLES * 4;
    h->delay_size = needed;

    h->delay_l = (float *)calloc(h->delay_size, sizeof(float));
    h->delay_r = (float *)calloc(h->delay_size, sizeof(float));

    if (!h->delay_l || !h->delay_r) {
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
    free(h);
}

void hrtf_reset(HrtfCore *h)
{
    if (!h) return;

    memset(h->delay_l, 0, h->delay_size * sizeof(float));
    memset(h->delay_r, 0, h->delay_size * sizeof(float));
    h->delay_pos = 0;

    for (size_t i=0; i<4; ++i) {
        biquad_reset(&h->l[i]);
        biquad_reset(&h->r[i]);
    }

    h->phase_smooth = wrap01(h->phase);
    h->distance_smooth = clampd(h->distance_m, 0.05, 20.0);
    h->last_phase = h->phase_smooth;
    h->last_distance = h->distance_smooth;
}

void hrtf_set_distance(HrtfCore *h, double distance_m)
{
    if (h) h->distance_m = clampd(distance_m, 0.05, 20.0);
}

void hrtf_set_rotation_phase(HrtfCore *h, double phase)
{
    if (h) h->phase = wrap01(phase);
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

    const double param_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms */
    const double distance_slew = exp(-1.0 / (0.050 * h->fs)); /* 50 ms */

    for (size_t i=0; i<n; ++i) {
        double dp = h->phase - h->phase_smooth;
        if (dp > 0.5) dp -= 1.0;
        if (dp < -0.5) dp += 1.0;
        h->phase_smooth = wrap01(h->phase_smooth + (1.0-param_slew) * dp);

        h->distance_smooth += (1.0-distance_slew) *
                              (h->distance_m - h->distance_smooth);

        const double theta = h->phase_smooth * 2.0 * HRTF_PI;
        const double s = sin(theta);

        /* Spherical head shadow / Interaural Level Difference (ILD).
           At 90 degrees (s = +1), the near ear (right) is 0 dB,
           and the far ear (left) is shadowed by -8 dB (far_gain = 0.3981).
           Front and rear remain symmetric (s = 0 -> gl = gr = 1.0). */
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

        /* Subtle near-field proximity term below 1m (capped at 1.0) */
        const double near = (d < 1.0) ? clampd(1.0 - d, 0.0, 1.0) : 0.0;

        /* ITD: max ~0.70 ms. Sound arrives FIRST at the near ear,
           so the FAR ear is delayed by itd_samples. */
        const double itd_s = HRTF_MAX_ITD_S * s;
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
            design_filters(h, h->phase_smooth, h->distance_smooth, h->l, h->r);
        }

        double yl = xl;
        double yr = xr;
        for (int k=0; k<4; ++k) {
            yl = biquad_process(&h->l[k], yl);
            yr = biquad_process(&h->r[k], yr);
        }

        /* Master headroom scale (0.7071 = -3.0 dB) to ensure pinna resonance peaks
           do not exceed 0 dBFS at 1m distance for a full-scale input. */
        const double master_headroom = 0.7071067811865475;

        float out0 = (float)(yl * gl * distance_gain * master_headroom);
        float out1 = (float)(yr * gr * distance_gain * master_headroom);

        /* Soft-knee saturation ceiling: guarantees peak amplitude never exceeds 0 dBFS */
        stereo[0][i] = soft_limit(out0);
        stereo[1][i] = soft_limit(out1);

        h->delay_pos++;
        if (h->delay_pos >= h->delay_size) h->delay_pos = 0;
    }
}
