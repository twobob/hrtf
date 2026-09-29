
#include "hrtf_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

#define HRTF_PI 3.141592653589793238462643383279502884
#define HRTF_MAX_ITD_S 0.00070

/* Output level calibration (see distance_gains()).
   HRTF_MASTER_GAIN puts pink noise at the default position (2 m, front,
   15% Space) 2 dB below the input. HRTF_LEVEL_RISE_DB is how far the level
   may rise above that as the source comes closer than 2 m: the largest rise
   that keeps every ear, in every direction, at every Space and Ear Scale
   setting, below the input level for pink noise (with a 0.35 dB margin).
   HRTF_ROOM_ENERGY is the room path's pink-noise energy per unit send,
   relative to the direct path, used to keep Space loudness-neutral. All
   three were measured with long pink noise over the parameter grid;
   test_clap.c re-checks the guarantee. */
#ifndef HRTF_MASTER_GAIN
#define HRTF_MASTER_GAIN 0.66837   /* -3.500 dB */
#endif
#ifndef HRTF_LEVEL_RISE_DB
#define HRTF_LEVEL_RISE_DB 0.45    /* binding case: 5 cm, 45 deg, 130% ear, 100% Space */
#endif

#ifndef HRTF_ROOM_ENERGY
#define HRTF_ROOM_ENERGY 0.293
#endif

static const double k_master_gain = HRTF_MASTER_GAIN;
static const double k_level_rise_db = HRTF_LEVEL_RISE_DB;
static const double k_room_energy = HRTF_ROOM_ENERGY;

typedef struct {
    double b0,b1,b2,a1,a2;
    double z1,z2;
} Biquad;

struct HrtfCore {
    double fs;
    double distance_m;
    double phase;
    double elevation_deg;
    double space;
    double phase_smooth;
    double distance_smooth;
    double elevation_smooth;
    double space_smooth;
    double ear_scale;
    double ear_scale_smooth;
    double reflections;        /* 1 = room reflections on, 0 = bypassed */
    double reflections_smooth;

    /* distance_gains() result for cached_distance */
    double cached_distance;
    double cached_direct_gain;
    double cached_room_gain;

    float *delay_l;
    float *delay_r;
    size_t delay_size;
    size_t delay_pos;

    float *early_buf;
    size_t early_size;
    size_t early_pos;
    double early_lpf_l;
    double early_lpf_r;

    /* Samples since the last coefficient redesign. Kept across calls so the
       render does not depend on how the host splits audio into blocks. */
    unsigned int ctrl_count;

    Biquad l[5];
    Biquad r[5];
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

/* Clamp a design frequency into [lo, hi] and then below max_f. Applying the
   Nyquist guard last keeps it authoritative even when max_f < lo, which
   happens at very low sample rates. */
static double clamp_freq(double f, double lo, double hi, double max_f)
{
    f = clampd(f, lo, hi);
    return f > max_f ? max_f : f;
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
    if (delay < 0.0) delay = 0.0;

    /* While hrtf_process() always passes delay >= 2.0 (incorporating the 2-sample
       PDC latency guard so all 4 Hermite taps remain strictly in the past of write_pos),
       this fallback protects direct callers of interp_delay() with delay < 1.0 from
       sampling unwritten/stale buffer data at tap i0 + 2. */
    if (delay < 1.0) {
        if (delay <= 0.0) return (double)buf[(size_t)write_pos];
        const size_t iw = (size_t)write_pos;
        const size_t im1 = (iw + size - 1) % size;
        return (1.0 - delay) * (double)buf[iw] + delay * (double)buf[im1];
    }

    double p = write_pos - delay;
    while (p < 0.0) p += (double)size;
    while (p >= (double)size) p -= (double)size;

    const size_t i0 = (size_t)p;
    const size_t im1 = (i0 + size - 1) % size;
    const size_t i1  = (i0 + 1) % size;
    const size_t i2  = (i0 + 2) % size;
    const double f = p - (double)i0;

    const double y_m1 = (double)buf[im1];
    const double y_0  = (double)buf[i0];
    const double y_1  = (double)buf[i1];
    const double y_2  = (double)buf[i2];

    /* 4-point, 3rd-order Hermite spline interpolation for flat frequency response up to Nyquist */
    const double c0 = y_0;
    const double c1 = 0.5 * (y_1 - y_m1);
    const double c2 = y_m1 - 2.5 * y_0 + 2.0 * y_1 - 0.5 * y_2;
    const double c3 = 0.5 * (y_2 - y_m1) + 1.5 * (y_0 - y_1);

    return ((c3 * f + c2) * f + c1) * f + c0;
}

/* Linearly interpolated read `delay` samples behind write_pos. Callers pass
   delay >= 1, so both taps are already written. Fractional taps let the
   reflection delays move continuously instead of jumping a whole sample. */
static double early_tap(const float *buf, size_t size, size_t write_pos, double delay)
{
    double p = (double)write_pos - delay;
    while (p < 0.0) p += (double)size;
    const size_t i0 = (size_t)p;
    const size_t i1 = (i0 + 1) % size;
    const double f = p - (double)i0;
    return (double)buf[i0 % size] + f * ((double)buf[i1] - (double)buf[i0 % size]);
}

/* Direct-path and room gains for a source at distance d (metres).

   The physical model gives the direct sound a 1/d law above 1 m and a
   gentle proximity term below it, while the room reflections decay only
   mildly with distance; their ratio (the direct-to-reverberant ratio) is the
   main distance cue and is preserved exactly here.

   The overall level is then re-shaped to follow a loudness curve anchored
   at 2 m: inverse distance beyond 2 m, and closer in a rise that eases into
   a ceiling HRTF_LEVEL_RISE_DB above the 2 m level (the tanh keeps the
   slope continuous at 2 m). Because the whole mix is re-scaled, the room
   gets quieter relative to the direct sound as the source approaches,
   exactly as the DRR demands, without the output ever getting louder than
   the input. */
static void distance_gains(double d, double *direct, double *room)
{
    const double dist = clampd(d, 0.05, 20.0);
    const double phys_direct = (dist >= 1.0) ? 1.0 / dist
                                             : 1.0 + (1.0 - dist) * 0.4142;
    const double phys_room = 1.0 / sqrt(1.0 + 0.15 * dist);

    double level;
    if (dist >= 2.0) {
        level = 2.0 / dist;
    } else if (k_level_rise_db > 1e-9) {
        const double halvings = log2(2.0 / dist);
        const double rise_db = k_level_rise_db * tanh(6.0206 * halvings / k_level_rise_db);
        level = pow(10.0, rise_db / 20.0);
    } else {
        level = 1.0;
    }

    *direct = k_master_gain * level;
    *room = k_master_gain * level * phys_room / phys_direct;
}

static void design_filters(HrtfCore *h, double phase, double elevation_deg, double distance,
                           double ear_scale, Biquad *l, Biquad *r)
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

    /* Graph-derived front presence around 3-5 kHz (+6 dB peak), reduced overhead/below */
    const double presence = 6.0 * front + 1.0 * rear + 1.5 * near - 3.0 * fabs(v);

    /* Anthropometric ear scaling:
       scale < 1.0 (smaller head/ears) shifts resonance and notch frequencies higher.
       scale > 1.0 (larger head/ears) shifts resonance and notch frequencies lower. */
    const double scale = clampd(ear_scale, 0.70, 1.30);
    const double inv_scale = 1.0 / scale;

    /* Protect against filter instability at low sample rates by keeping every
       design frequency below Nyquist. */
    const double max_f = 0.48 * h->fs;

    /* Dynamic elevation concha notch:
       Median plane notch shifts from ~3.5 kHz (below) up to ~9.5 kHz (overhead),
       scaled by anthropometric ear dimensions. */
    const double f_notch = clamp_freq((6500.0 + 3000.0 * v) * inv_scale, 3000.0, 15000.0, max_f);
    const double notch = -4.0 * rear - 3.5 * clampd(-v, 0.0, 1.0);

    /* High-frequency air falls towards rear, and undergoes cranial shadow from overhead. */
    const double overhead_shadow = clampd(v, 0.0, 1.0);
    const double air = 2.0 * front - 8.0 * rear - 1.5 * side + 1.0 * near - 3.0 * overhead_shadow;

    /* Side-specific head shadow / pinna asymmetry: the far ear loses more high frequency.
       When s > 0 (sound on right), left ear is far. When s < 0, right ear is far. */
    const double left_far  = clampd(s, 0.0, 1.0);
    const double right_far = clampd(-s, 0.0, 1.0);

    const double f_presence = clamp_freq(3900.0 * inv_scale, 1500.0, 7500.0, max_f);
    const double f_air = clamp_freq(8500.0 * inv_scale, 4000.0, 18000.0, max_f);
    const double f_side = clamp_freq(2200.0 * inv_scale, 1000.0, 4500.0, max_f);
    const double f_nf = clamp_freq(350.0 * inv_scale, 150.0, 800.0, max_f);

    biquad_peaking(&l[0], h->fs, f_presence, 0.85, presence);
    biquad_peaking(&r[0], h->fs, f_presence, 0.85, presence);

    biquad_peaking(&l[1], h->fs, f_notch, 3.0, notch);
    biquad_peaking(&r[1], h->fs, f_notch, 3.0, notch);

    biquad_highshelf(&l[2], h->fs, f_air, air - 9.0*left_far);
    biquad_highshelf(&r[2], h->fs, f_air, air - 9.0*right_far);

    /* A broad side cue keeps lateral positions from sounding like simple left/right panning */
    biquad_peaking(&l[3], h->fs, f_side, 0.9, -2.0*left_far);
    biquad_peaking(&r[3], h->fs, f_side, 0.9, -2.0*right_far);

    /* Near-field low-frequency ILD divergence (Distance Variation Function / DVF).
       In the near field (d < 1.0 m), spherical wavefront curvature produces significant
       low-frequency ILD (up to 9.5 dB). The whole difference is applied as a cut
       on the far ear: the interaural cue is the same, but the near ear is never
       pushed above the calibrated output level (see distance_gains()). */
    double nf_gain_l = 0.0;
    double nf_gain_r = 0.0;
    if (distance < 1.0) {
        const double d_norm = clampd(distance, 0.05, 1.0);
        const double prox = (1.0 - d_norm); /* 0 at 1m, 0.95 at 0.05m */
        const double nf_ild = 10.0 * prox * fabs(s); /* up to 9.5 dB difference */
        if (s > 0.0) {
            nf_gain_l = -nf_ild;
        } else if (s < 0.0) {
            nf_gain_r = -nf_ild;
        }
    }
    biquad_lowshelf(&l[4], h->fs, f_nf, nf_gain_l);
    biquad_lowshelf(&r[4], h->fs, f_nf, nf_gain_r);
}

HrtfCore *hrtf_create(double sample_rate)
{
    if (!(sample_rate > 1000.0)) return NULL;

    HrtfCore *h = (HrtfCore *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->fs = sample_rate;
    h->distance_m = h->distance_smooth = 2.0;
    h->phase = h->phase_smooth = 0.0;
    h->elevation_deg = h->elevation_smooth = 0.0;
    h->space = h->space_smooth = 0.15; /* 15% default room externalisation */
    h->ear_scale = h->ear_scale_smooth = 1.0; /* 100% standard anthropometric scale */
    h->reflections = h->reflections_smooth = 1.0;
    h->cached_distance = -1.0;

    /* Ensure delay line is sized to accommodate maximum near-field ITD (~1.46 ms)
       with margin at any sample rate up to 192 kHz and beyond. */
    size_t needed = (size_t)ceil(sample_rate * 0.0020) + 32;
    if (needed < 16) needed = 16;
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
    h->ctrl_count = 0;

    for (size_t i=0; i<5; ++i) {
        biquad_reset(&h->l[i]);
        biquad_reset(&h->r[i]);
    }

    h->phase_smooth = wrap01(h->phase);
    h->distance_smooth = clampd(h->distance_m, 0.05, 20.0);
    h->elevation_smooth = clampd(h->elevation_deg, -90.0, 90.0);
    h->space_smooth = clampd(h->space, 0.0, 1.0);
    h->ear_scale_smooth = clampd(h->ear_scale, 0.70, 1.30);
    h->reflections_smooth = h->reflections;
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

void hrtf_set_ear_scale(HrtfCore *h, double scale)
{
    if (!h || !isfinite(scale)) return;
    h->ear_scale = clampd(scale, 0.70, 1.30);
}

void hrtf_set_reflections(HrtfCore *h, int enabled)
{
    if (!h) return;
    h->reflections = enabled ? 1.0 : 0.0;
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

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    const unsigned int old_mxcsr = _mm_getcsr();
    _mm_setcsr(old_mxcsr | 0x8040); /* Enable FTZ (bit 15) and DAZ (bit 6) */
#endif

    /* Belt and braces: once a non-finite value reaches the filter state the
       output never recovers, so re-seed the smoothers if that ever happens. */
    if (!isfinite(h->phase_smooth)) h->phase_smooth = 0.0;
    if (!isfinite(h->distance_smooth)) h->distance_smooth = 2.0;
    if (!isfinite(h->elevation_smooth)) h->elevation_smooth = 0.0;
    if (!isfinite(h->space_smooth)) h->space_smooth = 0.15;
    if (!isfinite(h->ear_scale_smooth)) h->ear_scale_smooth = 1.0;
    if (!isfinite(h->reflections_smooth)) h->reflections_smooth = h->reflections;

    const double param_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms */
    const double distance_slew = exp(-1.0 / (0.050 * h->fs)); /* 50 ms */
    const double elev_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms */
    const double space_slew = exp(-1.0 / (0.030 * h->fs)); /* 30 ms */
    const double ear_scale_slew = exp(-1.0 / (0.040 * h->fs)); /* 40 ms */
    const double reflections_slew = exp(-1.0 / (0.020 * h->fs)); /* 20 ms fade */

    /* 1-pole wall absorption low-pass. The pole is derived from a fixed
       corner (~8 kHz, a pole of 0.35 at 48 kHz) so the room colour does not
       change with the host sample rate. */
    const double wall_pole = exp(-2.0 * HRTF_PI * 8020.1 / h->fs);

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

        h->ear_scale_smooth += (1.0-ear_scale_slew) *
                               (h->ear_scale - h->ear_scale_smooth);

        h->reflections_smooth += (1.0-reflections_slew) *
                                 (h->reflections - h->reflections_smooth);

        const double theta = h->phase_smooth * 2.0 * HRTF_PI;
        const double phi = h->elevation_smooth * (HRTF_PI / 180.0);
        const double cos_phi = cos(phi);
        const double sin_phi = sin(phi);
        const double s = cos_phi * sin(theta); /* Lateral projection: -1=left, +1=right */
        const double c = cos_phi * cos(theta); /* Front/Rear projection: +1=front, -1=rear */
        const double v = sin_phi;              /* Vertical projection: +1=overhead, -1=below */

        /* Spherical head shadow / Interaural Level Difference (ILD).
           At low frequencies (<500 Hz), acoustic diffraction allows sound to bend
           around the cranial sphere with minimal loss (far_gain = 0.6310 = -4.0 dB).
           Deep high-frequency shadowing is handled dynamically by the
           pinna/air high-shelf cascade on the contralateral ear (-9 dB additional). */
        const double far_gain = 0.630957344; /* -4.0 dB low-frequency diffraction shadow */
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

        /* Distance level and direct-to-reverberant balance, recomputed only
           when the smoothed distance actually moves. */
        const double d = h->distance_smooth;
        if (d != h->cached_distance) {
            distance_gains(d, &h->cached_direct_gain, &h->cached_room_gain);
            h->cached_distance = d;
        }
        const double direct_gain = h->cached_direct_gain;
        const double room_gain = h->cached_room_gain;

        /* Near-field ITD correction: spherical wavefront curvature increases
           effective acoustic path length to far ear at distances < 1m.
           Subtract offset at d = 1m so scale transitions continuously to 1.0 with zero jump. */
        const double nf_itd_offset = 0.00765 / (2.0 + 0.00765);
        const double nf_itd_scale = (d < 1.0) ? (1.0 + (0.00765 / (2.0 * d * d + 0.00765) - nf_itd_offset)) : 1.0;

        /* ITD calculation based on Woodworth's spherical head acoustic ray-tracing.
           Eliminates the ~0.12 ms over-estimate of sinusoidal models at intermediate angles (30°-60°). */
        double abs_s = fabs(s);
        if (abs_s > 1.0) abs_s = 1.0;
        const double theta_lat = asin(abs_s);
        /* sin(theta_lat) == abs_s by construction. */
        const double woodworth_scale = (abs_s + theta_lat) / (1.0 + 0.5 * HRTF_PI);
        const double signed_woodworth = (s >= 0.0) ? woodworth_scale : -woodworth_scale;

        const double itd_s = HRTF_MAX_ITD_S * signed_woodworth * nf_itd_scale * h->ear_scale_smooth;
        const double itd_samples = itd_s * h->fs;
        double dl = 0.0, dr = 0.0;
        if (itd_samples >= 0.0) dl = itd_samples;  /* source on right -> delay left ear */
        else dr = -itd_samples;                    /* source on left -> delay right ear */

        /* A single NaN or infinity reaching the recursive filters would
           latch them and silence the output for the rest of the session. */
        float x = mono[i];
        if (!isfinite(x)) x = 0.0f;
        h->delay_l[h->delay_pos] = x;
        h->delay_r[h->delay_pos] = x;

        /* 2-sample delay line read guard: guarantees all 4 Hermite spline taps
           (y_m1, y_0, y_1, y_2) access strictly causal, written samples. */
        const double delay_guard = 2.0;

        const double xl = interp_delay(h->delay_l, h->delay_size,
                                       (double)h->delay_pos, dl + delay_guard);
        const double xr = interp_delay(h->delay_r, h->delay_size,
                                       (double)h->delay_pos, dr + delay_guard);

        /* Re-design coefficients at a modest control rate. The parameter
           smoother runs at audio rate; 16-sample coefficient updates keep
           zippering negligible while avoiding per-sample trig/coefficient work.
           The counter persists across calls, so the update grid is the same
           whatever block sizes the host uses. */
        if (h->ctrl_count == 0u) {
            design_filters(h, h->phase_smooth, h->elevation_smooth, h->distance_smooth,
                           h->ear_scale_smooth, h->l, h->r);
        }
        h->ctrl_count = (h->ctrl_count + 1u) & 15u;

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
            const double fs = h->fs;

            /* Base boundary reflection delay times, in samples */
            const double t_floor = 0.0048 * fs;
            const double t_ceil  = 0.0081 * fs;
            const double t_wall  = 0.0140 * fs;
            const double t_rear  = 0.0234 * fs;

            /* Interaural reflection delays:
               Reflections arriving from lateral boundaries exhibit ITD (~0.65 ms * ear_scale).
               This decorrelates the binaural reflection field while maintaining
               exact Left-Right mathematical symmetry on the median plane (s = 0).
               The delays are fractional so they glide with rotation and ear
               scale automation instead of stepping a whole sample at a time. */
            const double wall_itd = 0.00065 * fs * h->ear_scale_smooth;
            const double lateral_itd = fabs(s) * 0.00040 * fs * h->ear_scale_smooth;
            const double lat_l = (s > 0.0) ? lateral_itd : 0.0;
            const double lat_r = (s < 0.0) ? lateral_itd : 0.0;

            /* 3D Direction-dependent early reflections:
               Incident energy hitting boundary surfaces scales with source direction cosines:
               - Floor/ceiling modulated by vertical projection v
               - Left/right walls modulated by lateral projection s
               - Rear wall modulated by front/back projection c */
            const double g_floor = 0.22 * (1.0 - 0.40 * v);
            const double g_ceil  = 0.20 * (1.0 + 0.40 * v);
            const double g_lwall = 0.20 * (1.0 - 0.60 * s);
            const double g_rwall = 0.20 * (1.0 + 0.60 * s);
            const double g_rear  = 0.16 * (1.0 - 0.50 * c);

            const double r1_l = early_tap(h->early_buf, sz, pos, t_floor + lat_l) * g_floor;
            const double r1_r = early_tap(h->early_buf, sz, pos, t_floor + lat_r) * g_floor;
            const double r2_l = early_tap(h->early_buf, sz, pos, t_ceil + lat_l) * g_ceil;
            const double r2_r = early_tap(h->early_buf, sz, pos, t_ceil + lat_r) * g_ceil;
            /* Left wall reaches the right ear later; right wall reaches the left ear later. */
            const double r3_l = early_tap(h->early_buf, sz, pos, t_wall) * g_lwall;
            const double r3_r = early_tap(h->early_buf, sz, pos, t_wall + wall_itd) * g_lwall;
            const double r4_l = early_tap(h->early_buf, sz, pos, t_wall + wall_itd) * g_rwall;
            const double r4_r = early_tap(h->early_buf, sz, pos, t_wall) * g_rwall;
            const double r5_l = early_tap(h->early_buf, sz, pos, t_rear + lat_l) * g_rear;
            const double r5_r = early_tap(h->early_buf, sz, pos, t_rear + lat_r) * g_rear;

            /* Binaural distribution: lateral wall reflections exhibit acoustic ILD and ITD at ears.
               Each ear sums its near wall first and the far wall second, so on
               the median plane (s = 0) both sums are bit-identical. */
            const double raw_l = (r1_l + r2_l + r5_l) + (r3_l + 0.25 * r4_l);
            const double raw_r = (r1_r + r2_r + r5_r) + (r4_r + 0.25 * r3_r);

            /* Gentle 1-pole wall absorption filter */
            h->early_lpf_l = (1.0 - wall_pole) * raw_l + wall_pole * h->early_lpf_l;
            h->early_lpf_r = (1.0 - wall_pole) * raw_r + wall_pole * h->early_lpf_r;

            h->early_pos++;
            if (h->early_pos >= h->early_size) h->early_pos = 0;
        }

        /* Room send: Space sets the amount, the Reflections switch fades the
           whole room in or out without touching the Space setting, and
           room_gain carries the distance-dependent direct-to-reverberant
           ratio (see distance_gains()). */
        const double space_gain = h->space_smooth * 0.70 * h->reflections_smooth;

        /* Space is loudness-neutral: adding room shifts the direct/room
           balance but does not raise the overall level, which stays on the
           distance curve. HRTF_ROOM_ENERGY is the room's broadband energy per
           unit send relative to the direct path (measured with pink noise). */
        const double wet = space_gain * room_gain / direct_gain;
        const double space_comp = 1.0 / sqrt(1.0 + k_room_energy * wet * wet);

        float out0 = (float)((yl * gl * direct_gain + h->early_lpf_l * space_gain * room_gain) * space_comp);
        float out1 = (float)((yr * gr * direct_gain + h->early_lpf_r * space_gain * room_gain) * space_comp);

        /* Soft-knee saturation ceiling: guarantees peak amplitude never exceeds
           0 dBFS. With the level calibration above it only engages when a hot
           input meets a filter boost. */
        stereo[0][i] = soft_limit(out0);
        stereo[1][i] = soft_limit(out1);

        h->delay_pos++;
        if (h->delay_pos >= h->delay_size) h->delay_pos = 0;
    }

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_setcsr(old_mxcsr);
#endif
}

/* -------------------------------------------------------------------------
   Test Signal Generator: Calibrated Pulses, Pink Noise, Sine & Dirac Clicks
   ------------------------------------------------------------------------- */

void hrtf_test_gen_init(HrtfTestGen *gen, double sample_rate)
{
    if (!gen) return;
    memset(gen, 0, sizeof(*gen));
    gen->sample_rate = sample_rate > 1000.0 ? sample_rate : 48000.0;
    gen->prng_state = 0x5A17F00Du;
    gen->tone = 0.5; /* Default: balanced pink noise */
    gen->mode = HRTF_TEST_MODE_NOISE;
    gen->freq_hz = 1000.0;
    gen->pulse_dur_s = 0.200;
}

void hrtf_test_gen_set_tone(HrtfTestGen *gen, double tone)
{
    if (!gen || !isfinite(tone)) return;
    gen->tone = clampd(tone, 0.0, 1.0);
}

void hrtf_test_gen_set_mode(HrtfTestGen *gen, int mode)
{
    if (!gen) return;
    if (mode < 0 || mode > 2) mode = HRTF_TEST_MODE_NOISE;
    gen->mode = mode;
}

void hrtf_test_gen_set_frequency(HrtfTestGen *gen, double freq_hz)
{
    if (!gen || !isfinite(freq_hz)) return;
    gen->freq_hz = clampd(freq_hz, 20.0, 20000.0);
}

void hrtf_test_gen_set_duration(HrtfTestGen *gen, double dur_s)
{
    if (!gen || !isfinite(dur_s)) return;
    gen->pulse_dur_s = clampd(dur_s, 0.001, 2.0);
}

void hrtf_test_gen_process(HrtfTestGen *gen, float *out_mono, size_t n, double bpm, double beat_pos, int is_playing)
{
    if (!gen || !out_mono || n == 0) return;

    if (bpm < 20.0 || bpm > 400.0 || !isfinite(bpm))
        bpm = 120.0;

    const double fs = gen->sample_rate;
    const double sec_per_beat = 60.0 / bpm;
    const double samples_per_beat = fs * sec_per_beat;

    /* Base duration, adjusted by tone if noise mode */
    double base_dur = (gen->pulse_dur_s > 0.0) ? gen->pulse_dur_s : 0.200;
    double pulse_dur = base_dur;
    double t_att = 0.005; /* default 5 ms attack */
    double t_rel = 0.005; /* default 5 ms release */

    const double tone = clampd(gen->tone, 0.0, 1.0);
    const int mode = gen->mode;

    if (mode == HRTF_TEST_MODE_NOISE) {
        if (tone < 0.5) {
            /* Low rumble: longer duration, smoother attack */
            const double r = tone * 2.0; /* 0.0 = full rumble, 1.0 = pink noise */
            pulse_dur = base_dur * (1.25 - 0.25 * r);
            t_att = 0.012 - 0.007 * r; /* 12 ms down to 5 ms */
            t_rel = 0.015 - 0.010 * r; /* 15 ms down to 5 ms */
        } else {
            /* Crisp transient: shorter duration, snappy attack */
            const double t = (tone - 0.5) * 2.0; /* 0.0 = pink noise, 1.0 = crisp click */
            pulse_dur = base_dur * (1.0 - 0.65 * t); /* down to ~70 ms */
            t_att = 0.005 - 0.004 * t; /* down to 1 ms */
            t_rel = 0.005;
        }
    } else if (mode == HRTF_TEST_MODE_SINE) {
        t_att = 0.005;
        t_rel = 0.005;
    }

    if (pulse_dur > 0.75 * sec_per_beat)
        pulse_dur = 0.75 * sec_per_beat;

    /* A pulse shorter than its attack plus release would never reach the
       release ramp and would be cut off mid-attack with a click. Shrink both
       ramps proportionally so the envelope always closes smoothly. */
    if (t_att + t_rel > pulse_dur) {
        const double k = pulse_dur / (t_att + t_rel);
        t_att *= k;
        t_rel *= k;
    }

    const double beats_per_sample = bpm / (60.0 * fs);
    const float rumble_coeff = (float)(1.0 - exp(-2.0 * HRTF_PI * 160.0 / fs)); /* ~160 Hz */

    /* Keep the sine burst below Nyquist at any sample rate. */
    double sine_freq = gen->freq_hz > 20.0 ? gen->freq_hz : 1000.0;
    if (sine_freq > 0.45 * fs) sine_freq = 0.45 * fs;

    /* A tempo change can leave the free-running counter beyond the new beat length. */
    if (!is_playing && gen->free_sample_counter >= samples_per_beat)
        gen->free_sample_counter = fmod(gen->free_sample_counter, samples_per_beat);

    /* Steepest per-sample change a raised-cosine ramp of length T can make is
       pi / (2 T fs). Limiting the envelope to (just above) that leaves every
       normal pulse untouched, but turns a jump into the middle of a pulse
       (playback starting mid-pulse, a loop or locate, a tempo change) into
       a ramp as smooth as the pulse's own attack instead of a click. */
    const double t_ramp_min = (t_att < t_rel) ? t_att : t_rel;
    const float env_max_step = (float)(1.01 * HRTF_PI / (2.0 * t_ramp_min * fs));

    for (size_t i = 0; i < n; ++i) {
        double t_in_beat = 0.0;
        int is_beat_start_sample = 0;

        if (is_playing) {
            double current_beat = beat_pos + (double)i * beats_per_sample;
            double phase = current_beat - floor(current_beat);
            t_in_beat = phase * sec_per_beat;
            double prev_counter = gen->free_sample_counter;
            gen->free_sample_counter = phase * samples_per_beat;
            if (gen->free_sample_counter < prev_counter || (size_t)gen->free_sample_counter == 0) {
                is_beat_start_sample = 1;
            }
        } else {
            t_in_beat = gen->free_sample_counter / fs;
            if ((size_t)gen->free_sample_counter == 0) {
                is_beat_start_sample = 1;
            }
            gen->free_sample_counter += 1.0;
            if (gen->free_sample_counter >= samples_per_beat)
                gen->free_sample_counter -= samples_per_beat;
        }

        if (mode == HRTF_TEST_MODE_CLICK) {
            /* Dirac click impulse at onset */
            out_mono[i] = is_beat_start_sample ? 0.85f : 0.0f;
            continue;
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
        if (envelope > gen->env + env_max_step) envelope = gen->env + env_max_step;
        else if (envelope < gen->env - env_max_step) envelope = gen->env - env_max_step;
        gen->env = envelope;

        if (mode == HRTF_TEST_MODE_SINE) {
            /* Sine burst at freq_hz */
            float s = (float)sin(gen->sine_phase);
            gen->sine_phase += 2.0 * HRTF_PI * sine_freq / fs;
            if (gen->sine_phase >= 2.0 * HRTF_PI) gen->sine_phase -= 2.0 * HRTF_PI;
            out_mono[i] = s * 0.25f * envelope;
            continue;
        }

        /* HRTF_TEST_MODE_NOISE: PRNG + Pink Filter + Tone Morphing */
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

        /* 1-pole low-pass rumble filter (~160 Hz cutoff) */
        gen->rumble_lpf += rumble_coeff * (white - gen->rumble_lpf);

        float sig = 0.0f;
        if (tone <= 0.5) {
            /* Blend rumble (tone=0.0) to pink noise (tone=0.5) */
            float r = (float)(tone * 2.0);
            float rumble = gen->rumble_lpf * 1.2f;
            sig = (1.0f - r) * rumble + r * (pink * 0.12f);
        } else {
            /* Blend pink noise (tone=0.5) to crisp transient/snap (tone=1.0) */
            float t = (float)((tone - 0.5) * 2.0);
            float crisp = (white - gen->b3) * 0.20f;
            sig = (1.0f - t) * (pink * 0.12f) + t * crisp;
        }

        out_mono[i] = sig * envelope;
    }
}

void hrtf_test_gen_render(HrtfTestGen *gen, const float *in, float *out, size_t n,
                          double bpm, double beat_pos, int is_playing, int enabled)
{
    if (!gen || !out || n == 0) return;

    const double fs = gen->sample_rate;
    const double target = enabled ? 1.0 : 0.0;

    /* Settled: pass the input through, or replace it outright. */
    if (gen->mix == target) {
        if (enabled) {
            hrtf_test_gen_process(gen, out, n, bpm, beat_pos, is_playing);
        } else if (in != out) {
            if (in) memmove(out, in, n * sizeof(float));
            else memset(out, 0, n * sizeof(float));
        }
        return;
    }

    /* Switching: crossfade between the input and the generator over 5 ms so
       ticking the Test Pulse box on or off never cuts a sound in half. The
       generator runs in small chunks on the stack, so `in` may alias `out`. */
    const double step = 1.0 / (0.005 * fs);
    const double beats_per_sample = (bpm >= 20.0 && bpm <= 400.0 && isfinite(bpm))
                                    ? bpm / (60.0 * fs) : 120.0 / (60.0 * fs);
    float chunk[64];
    size_t pos = 0;
    while (pos < n) {
        size_t m = n - pos;
        if (m > 64) m = 64;
        hrtf_test_gen_process(gen, chunk, m, bpm,
                              beat_pos + (double)pos * beats_per_sample, is_playing);
        for (size_t i = 0; i < m; ++i) {
            if (gen->mix < target) {
                gen->mix += step;
                if (gen->mix > target) gen->mix = target;
            } else if (gen->mix > target) {
                gen->mix -= step;
                if (gen->mix < target) gen->mix = target;
            }
            const float g = (float)gen->mix;
            const float x = in ? in[pos + i] : 0.0f;
            out[pos + i] = x + g * (chunk[i] - x);
        }
        pos += m;
    }
}
