/* 4-wide climate sampler.
 *
 * The CTS field fill and the exact window verification spend most of their
 * time in climate_sample(), i.e. in the per-octave Perlin evaluation.  The
 * scalar code is branch-free but completely serial per point, so the obvious
 * win is to evaluate four neighbouring quart positions at once.
 *
 * Correctness rules (the repository requires bit-identical results):
 *   - every lane performs exactly the same operations in the same order as
 *     the scalar samplePerlin()/sample_dp_range() code;
 *   - no FMA contraction (the build uses -ffp-contract=off);
 *   - the scalar implementation stays as the reference and as the fallback
 *     for non-x86 hosts (Emscripten) or CPUs without AVX2.
 *
 * The AVX2 path only handles shift == 0, which is what the field fill and
 * window_eval_rect() use.  Anything else falls back to the scalar code.
 */

#include "core.h"

#include <math.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define CTS_HAVE_AVX2_BUILD 1
#include <immintrin.h>
#else
#define CTS_HAVE_AVX2_BUILD 0
#endif

#if CTS_HAVE_AVX2_BUILD
#define CTS_AVX2 __attribute__((target("avx2")))
#endif

static const double kEdgeX[16] = {1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0,
                                  0.0, 0.0,  0.0, 0.0,  1.0, 0.0,  -1.0, 0.0};
static const double kEdgeY[16] = {1.0, 1.0, -1.0, -1.0, 0.0, 0.0, 0.0, 0.0,
                                  1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
static const double kEdgeZ[16] = {0.0, 0.0, 0.0, 0.0, 1.0, 1.0, -1.0, -1.0,
                                  1.0, 1.0, -1.0, -1.0, 0.0, 1.0, 0.0, -1.0};

static inline double poly1(double d)
{
    return d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
}

int cts_climate_simd_supported(void)
{
#if CTS_HAVE_AVX2_BUILD
    static int cached = -1;
    if (cached < 0) {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports("avx2") ? 1 : 0;
    }
    return cached;
#else
    return 0;
#endif
}

#if CTS_HAVE_AVX2_BUILD

CTS_AVX2 static inline __m256d poly4(__m256d d)
{
    const __m256d c6 = _mm256_set1_pd(6.0);
    const __m256d c15 = _mm256_set1_pd(15.0);
    const __m256d c10 = _mm256_set1_pd(10.0);
    __m256d t = _mm256_sub_pd(_mm256_mul_pd(d, c6), c15);
    t = _mm256_add_pd(_mm256_mul_pd(d, t), c10);
    __m256d d3 = _mm256_mul_pd(_mm256_mul_pd(d, d), d);
    return _mm256_mul_pd(d3, t);
}

CTS_AVX2 static inline __m256d lerp4(__m256d part, __m256d from, __m256d to)
{
    return _mm256_add_pd(from, _mm256_mul_pd(part, _mm256_sub_pd(to, from)));
}

CTS_AVX2 static inline __m256d grad_dot(const int vi[4], __m256d d1, __m256d d2, __m256d d3)
{
    __m256d gx = _mm256_set_pd(kEdgeX[vi[3]], kEdgeX[vi[2]], kEdgeX[vi[1]], kEdgeX[vi[0]]);
    __m256d gy = _mm256_set_pd(kEdgeY[vi[3]], kEdgeY[vi[2]], kEdgeY[vi[1]], kEdgeY[vi[0]]);
    __m256d gz = _mm256_set_pd(kEdgeZ[vi[3]], kEdgeZ[vi[2]], kEdgeZ[vi[1]], kEdgeZ[vi[0]]);
    __m256d t = _mm256_add_pd(_mm256_mul_pd(gx, d1), _mm256_mul_pd(gy, d2));
    return _mm256_add_pd(t, _mm256_mul_pd(gz, d3));
}

/* samplePerlin() with the d2 == 0 fast path used by every climate sample. */
CTS_AVX2 static __m256d perlin4(const PerlinNoise *p, __m256d xv, double z)
{
    const __m256d a = _mm256_set1_pd(p->a);
    __m256d d1 = _mm256_add_pd(xv, a);
    __m256d i1 = _mm256_floor_pd(d1);
    d1 = _mm256_sub_pd(d1, i1);
    __m256d t1 = poly4(d1);

    double d3 = z + p->c;
    double i3 = floor(d3);
    d3 -= i3;
    const int h3 = ((int)i3) & 0xff;
    const double t3 = poly1(d3);

    const int h2 = p->h2 & 0xff;
    const double d2 = p->d2;
    const double t2 = p->t2;

    int h1v[4];
    _mm_storeu_si128((__m128i *)h1v, _mm256_cvttpd_epi32(i1));

    int vidx[8][4];
    const uint8_t *idx = p->d;
    for (int l = 0; l < 4; ++l) {
        const int h1 = h1v[l] & 0xff;
        const int v2a = (uint8_t)(idx[h1] + h2);
        const int v2b = (uint8_t)(idx[h1 + 1] + h2);
        const int v3a = (uint8_t)(idx[v2a] + h3);
        const int v3b = (uint8_t)(idx[v2a + 1] + h3);
        const int v4a = (uint8_t)(idx[v2b] + h3);
        const int v4b = (uint8_t)(idx[v2b + 1] + h3);

        vidx[0][l] = idx[v3a] & 0xf;
        vidx[1][l] = idx[v3a + 1] & 0xf;
        vidx[2][l] = idx[v4a] & 0xf;
        vidx[3][l] = idx[v4a + 1] & 0xf;
        vidx[4][l] = idx[v3b] & 0xf;
        vidx[5][l] = idx[v3b + 1] & 0xf;
        vidx[6][l] = idx[v4b] & 0xf;
        vidx[7][l] = idx[v4b + 1] & 0xf;
    }

    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d d1m = _mm256_sub_pd(d1, one);
    const __m256d d2v = _mm256_set1_pd(d2);
    const __m256d d2m = _mm256_set1_pd(d2 - 1.0);
    const __m256d d3v = _mm256_set1_pd(d3);
    const __m256d d3m = _mm256_set1_pd(d3 - 1.0);

    __m256d l1 = grad_dot(vidx[0], d1, d2v, d3v);
    __m256d l5 = grad_dot(vidx[1], d1, d2v, d3m);
    __m256d l2 = grad_dot(vidx[2], d1m, d2v, d3v);
    __m256d l6 = grad_dot(vidx[3], d1m, d2v, d3m);
    __m256d l3 = grad_dot(vidx[4], d1, d2m, d3v);
    __m256d l7 = grad_dot(vidx[5], d1, d2m, d3m);
    __m256d l4 = grad_dot(vidx[6], d1m, d2m, d3v);
    __m256d l8 = grad_dot(vidx[7], d1m, d2m, d3m);

    l1 = lerp4(t1, l1, l2);
    l3 = lerp4(t1, l3, l4);
    l5 = lerp4(t1, l5, l6);
    l7 = lerp4(t1, l7, l8);

    l1 = lerp4(_mm256_set1_pd(t2), l1, l3);
    l5 = lerp4(_mm256_set1_pd(t2), l5, l7);

    return lerp4(_mm256_set1_pd(t3), l1, l5);
}

CTS_AVX2 static __m256d sample_dp_range4(const DoublePerlinNoise *dp, __m256d xv,
                                         double z, int from, int to)
{
    const double f337 = 337.0 / 331.0;
    __m256d v = _mm256_setzero_pd();
    int i;
    if (from < 0) from = 0;
    if (to > dp->octA.octcnt) to = dp->octA.octcnt;
    for (i = from; i < to; ++i) {
        const PerlinNoise *p = dp->octA.octaves + i;
        __m256d xl = _mm256_mul_pd(xv, _mm256_set1_pd(p->lacunarity));
        __m256d s = perlin4(p, xl, z * p->lacunarity);
        v = _mm256_add_pd(v, _mm256_mul_pd(_mm256_set1_pd(p->amplitude), s));
    }
    {
        int nb = dp->octB.octcnt;
        if (to > nb) to = nb;
        for (i = from; i < to; ++i) {
            const PerlinNoise *p = dp->octB.octaves + i;
            __m256d xl = _mm256_mul_pd(_mm256_mul_pd(xv, _mm256_set1_pd(p->lacunarity)),
                                       _mm256_set1_pd(f337));
            __m256d s = perlin4(p, xl, z * p->lacunarity * f337);
            v = _mm256_add_pd(v, _mm256_mul_pd(_mm256_set1_pd(p->amplitude), s));
        }
    }
    return _mm256_mul_pd(v, _mm256_set1_pd(dp->amplitude));
}

CTS_AVX2 static void climate_sample4_avx2(const Climate *c, const double qx[4], double qz,
                                          double e[4], double h[4])
{
    const __m256d xv = _mm256_loadu_pd(qx);
    const __m256d ev = sample_dp_range4(&c->bn.climate[NP_EROSION], xv, qz, 0, 99);
    const __m256d hv = sample_dp_range4(&c->bn.climate[NP_HUMIDITY], xv, qz, 0, 99);
    _mm256_storeu_pd(e, ev);
    _mm256_storeu_pd(h, hv);
}

CTS_AVX2 static void climate_low_h4_avx2(const Climate *c, const double qx[4], double qz,
                                         double e[4], double h[4])
{
    const __m256d xv = _mm256_loadu_pd(qx);
    const __m256d ev = sample_dp_range4(&c->bn.climate[NP_EROSION], xv, qz, 0, 2);
    const __m256d hv = sample_dp_range4(&c->bn.climate[NP_HUMIDITY], xv, qz, 0, 99);
    _mm256_storeu_pd(e, ev);
    _mm256_storeu_pd(h, hv);
}

#endif /* CTS_HAVE_AVX2_BUILD */

void climate_sample4(const Climate *c, const double qx[4], double qz, int shift,
                     double e[4], double h[4])
{
    if (shift == 0 && cts_climate_simd_supported()) {
#if CTS_HAVE_AVX2_BUILD
        climate_sample4_avx2(c, qx, qz, e, h);
        return;
#endif
    }
    for (int i = 0; i < 4; ++i)
        climate_sample(c, qx[i], qz, shift, &e[i], &h[i]);
}
void climate_low_h4(const Climate *c, const double qx[4], double qz,
                    double e[4], double h[4])
{
    if (cts_climate_simd_supported()) {
#if CTS_HAVE_AVX2_BUILD
        climate_low_h4_avx2(c, qx, qz, e, h);
        return;
#endif
    }
    for (int i = 0; i < 4; ++i)
        climate_low_h(c, qx[i], qz, &e[i], &h[i]);
}
