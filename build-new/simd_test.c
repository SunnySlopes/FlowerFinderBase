#include "core.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t next_u64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

int main(int argc, char **argv)
{
    const long rounds = argc > 1 ? atol(argv[1]) : 200000;
    Climate c;
    climate_init(&c, 12345, 0);

    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    long bad = 0, bad_low = 0;
    for (long i = 0; i < rounds; ++i) {
        int64_t bx = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        int64_t bz = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        double qx[4], e4[4], h4[4], es[4], hs[4];
        for (int k = 0; k < 4; ++k)
            qx[k] = (double)(bx + k);
        climate_sample4(&c, qx, (double)bz, 0, e4, h4);
        for (int k = 0; k < 4; ++k)
            climate_sample(&c, qx[k], (double)bz, 0, &es[k], &hs[k]);
        for (int k = 0; k < 4; ++k) {
            if (memcmp(&e4[k], &es[k], sizeof(double)) != 0 ||
                memcmp(&h4[k], &hs[k], sizeof(double)) != 0) {
                if (bad < 5)
                    printf("MISMATCH sample bx=%lld bz=%lld k=%d e=%.17g/%.17g h=%.17g/%.17g\n",
                           (long long)(bx + k), (long long)bz, k, e4[k], es[k], h4[k], hs[k]);
                ++bad;
            }
        }

        climate_low_h4(&c, qx, (double)bz, e4, h4);
        for (int k = 0; k < 4; ++k)
            climate_low_h(&c, qx[k], (double)bz, &es[k], &hs[k]);
        for (int k = 0; k < 4; ++k) {
            if (memcmp(&e4[k], &es[k], sizeof(double)) != 0 ||
                memcmp(&h4[k], &hs[k], sizeof(double)) != 0) {
                if (bad_low < 5)
                    printf("MISMATCH low bx=%lld bz=%lld k=%d e=%.17g/%.17g h=%.17g/%.17g\n",
                           (long long)(bx + k), (long long)bz, k, e4[k], es[k], h4[k], hs[k]);
                ++bad_low;
            }
        }
    }

    const long bench = argc > 2 ? atol(argv[2]) : 2000000;
    double e[4], h[4], es, hs;
    double sink = 0.0;
    rng = 0x123456789abcdefULL;
    double t0 = now_sec();
    for (long i = 0; i < bench; ++i) {
        int64_t bx = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        int64_t bz = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        double qx[4];
        for (int k = 0; k < 4; ++k) qx[k] = (double)(bx + k);
        climate_sample4(&c, qx, (double)bz, 0, e, h);
        sink += e[0] + h[3];
    }
    double t1 = now_sec();
    for (long i = 0; i < bench; ++i) {
        int64_t bx = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        int64_t bz = (int64_t)(next_u64(&rng) % 16000001ULL) - 8000000;
        for (int k = 0; k < 4; ++k) {
            climate_sample(&c, (double)(bx + k), (double)bz, 0, &es, &hs);
            sink += es + hs;
        }
    }
    double t2 = now_sec();
    printf("simd=%d rounds=%ld bad_sample=%ld bad_low=%ld\n",
           cts_climate_simd_supported(), rounds, bad, bad_low);
    printf("bench %ld quads: simd=%.3fs (%.1f ns/point) scalar=%.3fs (%.1f ns/point) speedup=%.2fx sink=%.1f\n",
           bench, t1 - t0, (t1 - t0) * 1e9 / (bench * 4),
           t2 - t1, (t2 - t1) * 1e9 / (bench * 4),
           (t2 - t1) / (t1 - t0), sink);
    return (bad || bad_low) ? 1 : 0;
}
