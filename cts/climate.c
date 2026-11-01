#include "core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define TMP (&c->bn.climate[NP_TEMPERATURE])
#define ERO (&c->bn.climate[NP_EROSION])
#define HUM (&c->bn.climate[NP_HUMIDITY])
#define CNT (&c->bn.climate[NP_CONTINENTALNESS])
#define WDR (&c->bn.climate[NP_WEIRDNESS])
#define SHF (&c->bn.climate[NP_SHIFT])

static const double F337 = 337.0 / 331.0;

int64_t floor_div4(int64_t v)
{
    return (v >= 0) ? (v / 4) : (-((-v + 3) / 4));
}

double now_sec(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

int climate_init(Climate *c, uint64_t seed, int large)
{
    memset(c, 0, sizeof(*c));
    initBiomeNoise(&c->bn, MC_NEWEST);
    setBiomeSeed(&c->bn, seed, large ? 1 : 0);
    c->seed = seed;
    c->large = large ? 1 : 0;
    return 1;
}

/* Sum of a contiguous range of active octave pairs of a double perlin,
 * sampled at fixed y=0, including the double-perlin normalization. */
static double sample_dp_range(const DoublePerlinNoise *dp,
                              double x, double z, int from, int to)
{
    double v = 0.0;
    int i;
    if (from < 0) from = 0;
    if (to > dp->octA.octcnt) to = dp->octA.octcnt;
    for (i = from; i < to; i++) {
        const PerlinNoise *p = dp->octA.octaves + i;
        double lf = p->lacunarity;
        v += p->amplitude * samplePerlin(p, x * lf, 0.0, z * lf, 0.0, 0.0);
    }
    {
        int nb = dp->octB.octcnt;
        if (to > nb) to = nb;
        for (i = from; i < to; i++) {
            const PerlinNoise *p = dp->octB.octaves + i;
            double lf = p->lacunarity;
            v += p->amplitude * samplePerlin(p, x * lf * F337, 0.0,
                                             z * lf * F337, 0.0, 0.0);
        }
    }
    return v * dp->amplitude;
}

double climate_e(const Climate *c, double qx, double qz, int shift)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    return sample_dp_range(ERO, px, pz, 0, 99);
}

double climate_e_low(const Climate *c, double qx, double qz)
{
    return sample_dp_range(ERO, qx, qz, 0, 2);
}

double climate_e_high(const Climate *c, double qx, double qz)
{
    return sample_dp_range(ERO, qx, qz, 2, 99);
}

double climate_h(const Climate *c, double qx, double qz)
{
    return sample_dp_range(HUM, qx, qz, 0, 99);
}

void climate_sample(const Climate *c, double qx, double qz, int shift,
                    double *e, double *h)
{
    double px = qx, pz = qz;
    if (shift) {
        /* cubiomes quirk: the second offset noise is sampled at (z, x, 0),
         * i.e. the y argument is qx, not 0.  Match it exactly. */
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    *e = sample_dp_range(ERO, px, pz, 0, 99);
    *h = sample_dp_range(HUM, px, pz, 0, 99);
}

void climate_sample5(const Climate *c, double qx, double qz, int shift,
                     double *t, double *h, double *cont, double *e, double *w)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    *t = sample_dp_range(TMP, px, pz, 0, 99);
    *h = sample_dp_range(HUM, px, pz, 0, 99);
    *cont = sample_dp_range(CNT, px, pz, 0, 99);
    *e = sample_dp_range(ERO, px, pz, 0, 99);
    *w = sample_dp_range(WDR, px, pz, 0, 99);
}

void climate_sample3(const Climate *c, double qx, double qz, int shift,
                     double *t, double *cont, double *w)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    *t = sample_dp_range(TMP, px, pz, 0, 99);
    *cont = sample_dp_range(CNT, px, pz, 0, 99);
    *w = sample_dp_range(WDR, px, pz, 0, 99);
}

double climate_t(const Climate *c, double qx, double qz, int shift)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    return sample_dp_range(TMP, px, pz, 0, 99);
}

double climate_c(const Climate *c, double qx, double qz, int shift)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    return sample_dp_range(CNT, px, pz, 0, 99);
}

double climate_w(const Climate *c, double qx, double qz, int shift)
{
    double px = qx, pz = qz;
    if (shift) {
        double sx = sampleDoublePerlin(SHF, qx, 0.0, qz);
        double sz = sampleDoublePerlin(SHF, qz, qx, 0.0);
        px = qx + sx * 4.0;
        pz = qz + sz * 4.0;
    }
    return sample_dp_range(WDR, px, pz, 0, 99);
}

void climate_low_h(const Climate *c, double qx, double qz,
                   double *e_low, double *h)
{
    *e_low = sample_dp_range(ERO, qx, qz, 0, 2);
    *h = sample_dp_range(HUM, qx, qz, 0, 99);
}


void window_eval_perimeter(const Climate *c, int qx, int qz, int size_quart,
                           int shift, WindowEval *w)
{
    const double big = 1e300;
    int i, m = size_quart - 1;
    w->e_min = big; w->e_max = -big;
    w->h_min = big; w->h_max = -big;
    w->e_min_qx = w->e_min_qz = w->e_max_qx = w->e_max_qz = -1;
    w->h_min_qx = w->h_min_qz = w->h_max_qx = w->h_max_qz = -1;
    w->pass = 0;
#define EDGE_SAMPLE(xx, zz) do { \
        double x_ = (double)(xx), z_ = (double)(zz); \
        double e_, h_; \
        climate_sample(c, x_, z_, shift, &e_, &h_); \
        if (e_ < w->e_min) { w->e_min = e_; w->e_min_qx = xx; w->e_min_qz = zz; } \
        if (e_ > w->e_max) { w->e_max = e_; w->e_max_qx = xx; w->e_max_qz = zz; } \
        if (h_ < w->h_min) { w->h_min = h_; w->h_min_qx = xx; w->h_min_qz = zz; } \
        if (h_ > w->h_max) { w->h_max = h_; w->h_max_qx = xx; w->h_max_qz = zz; } \
    } while (0)
    for (i = 0; i < m; i++) {
        EDGE_SAMPLE(qx + i, qz);
        EDGE_SAMPLE(qx + i, qz + m);
        EDGE_SAMPLE(qx, qz + i);
        EDGE_SAMPLE(qx + m, qz + i);
    }
    EDGE_SAMPLE(qx + m, qz);       /* top-right */
    EDGE_SAMPLE(qx, qz + m);       /* bottom-left */
    EDGE_SAMPLE(qx + m, qz + m);   /* bottom-right */
#undef EDGE_SAMPLE
}


void window_eval_perimeter_step(const Climate *c, int qx, int qz, int size_quart,
                                int step, int shift, WindowEval *w)
{
    const double big = 1e300;
    int i, m = size_quart - 1;
    if (step < 1) step = 1;
    w->e_min = big; w->e_max = -big;
    w->h_min = big; w->h_max = -big;
    w->e_min_qx = w->e_min_qz = w->e_max_qx = w->e_max_qz = -1;
    w->h_min_qx = w->h_min_qz = w->h_max_qx = w->h_max_qz = -1;
    w->pass = 0;
#define PSTEP(xx, zz) do { \
        double e_, h_; \
        climate_sample(c, (double)(xx), (double)(zz), shift, &e_, &h_); \
        if (e_ < w->e_min) { w->e_min = e_; w->e_min_qx = (xx); w->e_min_qz = (zz); } \
        if (e_ > w->e_max) { w->e_max = e_; w->e_max_qx = (xx); w->e_max_qz = (zz); } \
        if (h_ < w->h_min) { w->h_min = h_; w->h_min_qx = (xx); w->h_min_qz = (zz); } \
        if (h_ > w->h_max) { w->h_max = h_; w->h_max_qx = (xx); w->h_max_qz = (zz); } \
    } while (0)
    for (i = 0; i <= m; i += step) {
        PSTEP(qx + i, qz);
        PSTEP(qx + i, qz + m);
        PSTEP(qx, qz + i);
        PSTEP(qx + m, qz + i);
    }
    /* endpoints that the stride may have skipped */
    PSTEP(qx + m, qz);
    PSTEP(qx, qz + m);
    PSTEP(qx + m, qz + m);
#undef PSTEP
}

void window_eval_rect(const Climate *c, int qx, int qz, int winx, int winz,
                      int shift, WindowEval *w)
{
    int ix, iz;
    const double big = 1e300;
    w->e_min = big; w->e_max = -big;
    w->h_min = big; w->h_max = -big;
    w->e_min_qx = w->e_min_qz = w->e_max_qx = w->e_max_qz = -1;
    w->h_min_qx = w->h_min_qz = w->h_max_qx = w->h_max_qz = -1;
    w->pass = 0;
    for (iz = 0; iz < winz; iz++) {
        for (ix = 0; ix < winx; ix++) {
            double e, h;
            climate_sample(c, (double)(qx + ix), (double)(qz + iz),
                           shift, &e, &h);
            if (e < w->e_min) { w->e_min = e; w->e_min_qx = qx+ix; w->e_min_qz = qz+iz; }
            if (e > w->e_max) { w->e_max = e; w->e_max_qx = qx+ix; w->e_max_qz = qz+iz; }
            if (h < w->h_min) { w->h_min = h; w->h_min_qx = qx+ix; w->h_min_qz = qz+iz; }
            if (h > w->h_max) { w->h_max = h; w->h_max_qx = qx+ix; w->h_max_qz = qz+iz; }
        }
    }
}

void window_eval_perimeter_rect(const Climate *c, int qx, int qz,
                                int winx, int winz, int step, int shift,
                                WindowEval *w)
{
    const double big = 1e300;
    int i, mx = winx - 1, mz = winz - 1;
    if (step < 1) step = 1;
    w->e_min = big; w->e_max = -big;
    w->h_min = big; w->h_max = -big;
    w->e_min_qx = w->e_min_qz = w->e_max_qx = w->e_max_qz = -1;
    w->h_min_qx = w->h_min_qz = w->h_max_qx = w->h_max_qz = -1;
    w->pass = 0;
#define RSTEP(xx, zz) do { \
        double e_, h_; \
        climate_sample(c, (double)(xx), (double)(zz), shift, &e_, &h_); \
        if (e_ < w->e_min) { w->e_min = e_; w->e_min_qx = (xx); w->e_min_qz = (zz); } \
        if (e_ > w->e_max) { w->e_max = e_; w->e_max_qx = (xx); w->e_max_qz = (zz); } \
        if (h_ < w->h_min) { w->h_min = h_; w->h_min_qx = (xx); w->h_min_qz = (zz); } \
        if (h_ > w->h_max) { w->h_max = h_; w->h_max_qx = (xx); w->h_max_qz = (zz); } \
    } while (0)
    for (i = 0; i <= mx; i += step) {
        RSTEP(qx + i, qz);
        RSTEP(qx + i, qz + mz);
    }
    for (i = 0; i <= mz; i += step) {
        RSTEP(qx, qz + i);
        RSTEP(qx + mx, qz + i);
    }
    RSTEP(qx + mx, qz);
    RSTEP(qx, qz + mz);
    RSTEP(qx + mx, qz + mz);
#undef RSTEP
}

void window_eval(const Climate *c, int qx, int qz, int size_quart, int shift,
                 WindowEval *w)
{
    int ix, iz;
    const double big = 1e300;
    w->e_min = big; w->e_max = -big;
    w->h_min = big; w->h_max = -big;
    w->e_min_qx = w->e_min_qz = w->e_max_qx = w->e_max_qz = -1;
    w->h_min_qx = w->h_min_qz = w->h_max_qx = w->h_max_qz = -1;
    w->pass = 0;
    for (iz = 0; iz < size_quart; iz++) {
        for (ix = 0; ix < size_quart; ix++) {
            double x = (double)(qx + ix);
            double z = (double)(qz + iz);
            double e, h;
            climate_sample(c, x, z, shift, &e, &h);
            if (e < w->e_min) { w->e_min = e; w->e_min_qx = qx+ix; w->e_min_qz = qz+iz; }
            if (e > w->e_max) { w->e_max = e; w->e_max_qx = qx+ix; w->e_max_qz = qz+iz; }
            if (h < w->h_min) { w->h_min = h; w->h_min_qx = qx+ix; w->h_min_qz = qz+iz; }
            if (h > w->h_max) { w->h_max = h; w->h_max_qx = qx+ix; w->h_max_qz = qz+iz; }
        }
    }
}
