#include "core.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define BCACHE_SLOTS (1u << 20)
static _Atomic uint64_t bcache_key[BCACHE_SLOTS];
static _Atomic int bcache_val[BCACHE_SLOTS];
/* Per-thread hit/miss counters.  They are diagnostics only; keeping them in
 * shared atomics turned every cache lookup into a cache-line ping-pong
 * between the (parallel) verification threads. */
static _Thread_local uint64_t bcache_hits;
static _Thread_local uint64_t bcache_misses;

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t bcache_hash(const Generator *g, int x, int y, int z)
{
    uint64_t h = g->seed;
    h ^= (uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)(uint32_t)z * 0xC2B2AE3D27D4EB4FULL;
    h ^= (uint64_t)(uint32_t)y * 0x165667B19E3779F9ULL;
    h ^= (uint64_t)(uint32_t)g->dim * 0x27D4EB2F165667C5ULL;
    return splitmix64(h);
}

/* Same value as cubiomes getBiomeAt(g, 1, x, y, z), but without the
 * calloc/free per query: a 1x1x1 scale-1 sample needs 1 result int plus a
 * 3x3x3 voronoi source (28 ints for every version this tool supports), so a
 * small stack buffer covers the common case.  Verification performs tens of
 * thousands of these lookups per candidate. */
#define BIOME_SCRATCH_INTS 64
static int biome_sample_uncached(const Generator *g, int x, int y, int z)
{
    Range r = {1, x, z, 1, 1, y, 1};
    size_t len = getMinCacheSize(g, 1, 1, 1, 1);
    int local[BIOME_SCRATCH_INTS];
    int *buf = local;
    int id;
    if (len == 0 || len > (size_t)BIOME_SCRATCH_INTS) {
        buf = (int *)calloc(len ? len : 1, sizeof(int));
        if (!buf) return none;
    } else {
        memset(local, 0, len * sizeof(int));
    }
    id = genBiomes(g, buf, r);
    if (id == 0)
        id = buf[0];
    else
        id = none;
    if (buf != local)
        free(buf);
    return id;
}

int biome_at_cached(const Generator *g, int x, int y, int z)
{
    uint64_t h = bcache_hash(g, x, y, z);
    uint64_t tag = h ? h : 1;
    size_t idx = (size_t)h & (BCACHE_SLOTS - 1);
    uint64_t k = atomic_load_explicit(&bcache_key[idx], memory_order_acquire);
    int v;
    if (k == tag) {
        bcache_hits++;
        return atomic_load_explicit(&bcache_val[idx], memory_order_relaxed);
    }
    bcache_misses++;
    v = biome_sample_uncached(g, x, y, z);
    atomic_store_explicit(&bcache_val[idx], v, memory_order_relaxed);
    atomic_store_explicit(&bcache_key[idx], tag, memory_order_release);
    return v;
}

/* Per-thread counters; call from the thread whose lookups are being measured.
 * Kept for debugging only (the GUI does not report cache statistics). */
void biome_cache_stats(uint64_t *hits, uint64_t *misses)
{
    if (hits) *hits = bcache_hits;
    if (misses) *misses = bcache_misses;
}

int *biome_map(const Generator *g, int x, int z, int w, int h, int y, int scale)
{
    Range r;
    int *ids;
    if (scale <= 0 || w <= 0 || h <= 0 || w % scale || h % scale) return NULL;
    r.scale = scale;
    r.x = x; r.z = z;
    r.sx = w / scale;
    r.sz = h / scale;
    r.y = y; r.sy = 1;
    ids = allocCache(g, r);
    if (!ids) return NULL;
    if (genBiomes(g, ids, r) != 0) {
        free(ids);
        return NULL;
    }
    return ids;
}

/* Largest all-1 square of side k in a w x h mask. */
static int find_square(const unsigned char *m, int w, int h, int k, int *ox, int *oz)
{
    int *sum;
    int x, y, stride = w + 1;
    if (k < 1 || k > w || k > h) return 0;
    sum = (int *)calloc((size_t)(w + 1) * (h + 1), sizeof(int));
    if (!sum) return 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            sum[(y + 1) * stride + (x + 1)] =
                m[y * w + x]
                + sum[y * stride + (x + 1)]
                + sum[(y + 1) * stride + x]
                - sum[y * stride + x];
        }
    }
    for (y = 0; y <= h - k; y++) {
        for (x = 0; x <= w - k; x++) {
            int s = sum[(y + k) * stride + (x + k)]
                  - sum[y * stride + (x + k)]
                  - sum[(y + k) * stride + x]
                  + sum[y * stride + x];
            if (s == k * k) {
                *ox = x;
                *oz = y;
                free(sum);
                return 1;
            }
        }
    }
    free(sum);
    return 0;
}

/* 1.21.5 parameter boxes (normalized climate, y=0 quart):
 *   pale garden: E<0.05, H>0.30, T in [-0.15,0.20], |W| in [0.40,0.9334],
 *                C>0.30; 8x8 blocks = 2x2 cells.
 *   swamp:       E>0.55, C>-0.11, T<0.20; 8x8 blocks = 2x2 cells.
 *   flower:      H<-0.35, W<0, T in [-0.15,0.20]; 32x32 blocks = 8x8 cells.
 */
int biome_precheck(const Climate *c, int qx, int qz, int winx, int winz, int shift,
                   int require_pale)
{
    double tmin = 1e30, tmax = -1e30;
    double cmax = -1e30;
    double wmin = 1e30, wmax = -1e30, wabsmax = 0.0;
    int ix, iz, mx = winx - 1, mz = winz - 1;
    for (iz = 0; iz <= 2; iz++) {
        for (ix = 0; ix <= 2; ix++) {
            double t, cont, w;
            int x = (ix * mx) / 2, z = (iz * mz) / 2;
            climate_sample3(c, (double)(qx + x), (double)(qz + z),
                            shift, &t, &cont, &w);
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
            if (cont > cmax) cmax = cont;
            if (w < wmin) wmin = w;
            if (w > wmax) wmax = w;
            if (fabs(w) > wabsmax) wabsmax = fabs(w);
        }
    }
    /* Flower + swamp share a temperate band; flower also wants W < 0. */
    if (!(tmin < 0.22 && tmax > -0.17)) return 0;
    if (!(wmin < 0.02)) return 0;
    if (require_pale) {
        if (cmax < 0.28) return 0;
        if (!(wmax > 0.38 || wabsmax > 0.38)) return 0;
    }
    return 1;
}

int biome_pale_possible(const Climate *c, int qx, int qz, int winx, int winz,
                        int shift)
{
    int x, z;
    for (z = 0; z < winz; z += 2) {
        for (x = 0; x < winx; x += 2) {
            double e, h;
            climate_sample(c, (double)(qx + x), (double)(qz + z),
                           shift, &e, &h);
            if (e < 0.07 && h > 0.28)
                return 1;
        }
    }
    return 0;
}

/* Actual-ID variant: conditions select candidate cells, getBiomeAt at a high Y
 * builds the real biome masks, and patch sizes are checked on those masks. */
int biome_check_window_real(const Climate *c, const Generator *g, int qx, int qz,
                            int winx, int winz, int y, BiomeResult *res)
{
    int w = winx, h = winz;
    int x, z, px, pz;
    unsigned char *pmask, *smask, *fmask;
    if (!res) return 0;
    memset(res, 0, sizeof(*res));
    res->pale_qx = res->pale_qz = -1;
    res->swamp_qx = res->swamp_qz = -1;
    res->flower_qx = res->flower_qz = -1;
    if (w <= 0 || h <= 0) return 0;
    pmask = (unsigned char *)calloc((size_t)w * h, 1);
    smask = (unsigned char *)calloc((size_t)w * h, 1);
    fmask = (unsigned char *)calloc((size_t)w * h, 1);
    if (!pmask || !smask || !fmask) {
        free(pmask); free(smask); free(fmask);
        return 0;
    }
    for (z = 0; z < h; z++) {
        for (x = 0; x < w; x++) {
            double e, hh;
            size_t i = (size_t)z * w + x;
            climate_sample(c, (double)(qx + x), (double)(qz + z), 1, &e, &hh);
            if (!((e < 0.05 && hh > 0.30) || e > 0.55 || hh < -0.35))
                continue;
            {
                double t, cont, ww;
                int id;
                climate_sample3(c, (double)(qx + x), (double)(qz + z),
                                1, &t, &cont, &ww);
                if (!((e < 0.05 && hh > 0.30 && t >= -0.15 && t <= 0.20 &&
                       fabs(ww) >= 0.40 && fabs(ww) <= 0.9334 && cont > 0.30) ||
                      (e > 0.55 && cont > -0.11 && t < 0.20) ||
                      (hh < -0.35 && ww < 0.0 && t >= -0.15 && t <= 0.20)))
                    continue;
                id = biome_at_cached(g, (qx + x) * 4, y, (qz + z) * 4);
                if (id == pale_garden) pmask[i] = 1;
                if (id == swamp) smask[i] = 1;
                if (id == flower_forest) fmask[i] = 1;
            }
        }
    }
    res->pale = find_square(pmask, w, h, 2, &px, &pz);
    if (res->pale) { res->pale_qx = qx + px; res->pale_qz = qz + pz; }
    res->swamp = find_square(smask, w, h, 2, &px, &pz);
    if (res->swamp) { res->swamp_qx = qx + px; res->swamp_qz = qz + pz; }
    res->flower = find_square(fmask, w, h, 8, &px, &pz);
    if (res->flower) { res->flower_qx = qx + px; res->flower_qz = qz + pz; }
    free(pmask); free(smask); free(fmask);
    return 1;
}

static int actual_refine(const Generator *g, int bqx, int bqz, int k,
                         int y, int id, int *rx, int *rz)
{
    unsigned char mask[64];
    int x, z, px, pz, sq = (k == 8) ? 8 : 2;
    if (k > 8) return 0;
    for (z = 0; z < k; z++)
        for (x = 0; x < k; x++)
            mask[z * k + x] = biome_at_cached(g, (bqx + x) * 4, y,
                                                (bqz + z) * 4) == id;
    if (find_square(mask, k, k, sq, &px, &pz)) {
        *rx = bqx + px; *rz = bqz + pz;
        return 1;
    }
    return 0;
}

int biome_pale_actual_possible(const Climate *c, const Generator *g, int qx,
                               int qz, int winx, int winz, int y)
{
    int x, z;
    for (z = 0; z < winz; z += 4) {
        for (x = 0; x < winx; x += 4) {
            double e, h;
            climate_sample(c, (double)(qx + x), (double)(qz + z), 1, &e, &h);
            if (e < 0.08 && h > 0.27) {
                int id = biome_at_cached(g, (qx + x) * 4, y, (qz + z) * 4);
                if (id == pale_garden)
                    return 1;
            }
        }
    }
    return 0;
}

int biome_check_window_fast(const Climate *c, const Generator *g, int qx, int qz,
                            int winx, int winz, int y, int require_pale,
                            BiomeResult *res)
{
    int cw = winx / 2, ch = winz / 2;
    int x, z, px, pz, found;
    unsigned char *pmask, *smask, *fmask;
    if (!res || cw <= 0 || ch <= 0) return 0;
    memset(res, 0, sizeof(*res));
    res->pale_qx = res->pale_qz = -1;
    res->swamp_qx = res->swamp_qz = -1;
    res->flower_qx = res->flower_qz = -1;
    pmask = (unsigned char *)calloc((size_t)cw * ch, 1);
    smask = (unsigned char *)calloc((size_t)cw * ch, 1);
    fmask = (unsigned char *)calloc((size_t)cw * ch, 1);
    if (!pmask || !smask || !fmask) {
        free(pmask); free(smask); free(fmask);
        return 0;
    }
    for (z = 0; z < ch; z++) {
        for (x = 0; x < cw; x++) {
            double e, h;
            size_t i = (size_t)z * cw + x;
            climate_sample(c, (double)(qx + 2 * x), (double)(qz + 2 * z),
                           1, &e, &h);
            if ((e < 0.05 && h > 0.30) || e > 0.55 || h < -0.35) {
                double t, cont, w;
                climate_sample3(c, (double)(qx + 2 * x), (double)(qz + 2 * z),
                                1, &t, &cont, &w);
                if (e < 0.05 && h > 0.30 && t >= -0.15 && t <= 0.20 &&
                    fabs(w) >= 0.40 && fabs(w) <= 0.9334 && cont > 0.30)
                    pmask[i] = 1;
                if (e > 0.55 && cont > -0.11 && t < 0.20)
                    smask[i] = 1;
                if (h < -0.35 && w < 0.0 && t >= -0.15 && t <= 0.20)
                    fmask[i] = 1;
            }
        }
    }
    if (require_pale) {
        found = 0;
        for (z = 0; z < ch && !found; z++) {
            for (x = 0; x < cw; x++) {
                if (!pmask[(size_t)z * cw + x]) continue;
                if (actual_refine(g, qx + 2 * x - 1, qz + 2 * z - 1, 4, y,
                                  pale_garden, &px, &pz)) {
                    res->pale = 1;
                    res->pale_qx = px; res->pale_qz = pz;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) goto done;
    } else {
        /* Optional pale: record if present, but do not reject the window. */
        for (z = 0; z < ch; z++) {
            for (x = 0; x < cw; x++) {
                if (!pmask[(size_t)z * cw + x]) continue;
                if (actual_refine(g, qx + 2 * x - 1, qz + 2 * z - 1, 4, y,
                                  pale_garden, &px, &pz)) {
                    res->pale = 1;
                    res->pale_qx = px; res->pale_qz = pz;
                    goto pale_done;
                }
            }
        }
pale_done:;
    }
    found = 0;
    for (z = 0; z < ch && !found; z++) {
        for (x = 0; x < cw; x++) {
            if (!smask[(size_t)z * cw + x]) continue;
            if (actual_refine(g, qx + 2 * x - 1, qz + 2 * z - 1, 4, y,
                              swamp, &px, &pz)) {
                res->swamp = 1;
                res->swamp_qx = px; res->swamp_qz = pz;
                found = 1;
                break;
            }
        }
    }
    if (!found) goto done;
    found = 0;
    for (z = 0; z <= ch - 4 && !found; z++) {
        for (x = 0; x <= cw - 4; x++) {
            int all = 1, dx, dz;
            for (dz = 0; dz < 4 && all; dz++)
                for (dx = 0; dx < 4; dx++)
                    if (!fmask[(size_t)(z + dz) * cw + (x + dx)]) {
                        all = 0;
                        break;
                    }
            if (!all) continue;
            if (actual_refine(g, qx + 2 * x, qz + 2 * z, 8, y,
                              flower_forest, &px, &pz)) {
                res->flower = 1;
                res->flower_qx = px; res->flower_qz = pz;
                found = 1;
                break;
            }
        }
    }
done:
    free(pmask); free(smask); free(fmask);
    if (require_pale)
        return res->pale && res->swamp && res->flower;
    return res->swamp && res->flower;
}

int biome_check_window(const Climate *c, int qx, int qz, int winx, int winz,
                       int shift, BiomeResult *res)
{
    int w = winx, h = winz;
    int x, z, px, pz;
    unsigned char *pale, *swamp, *flower;
    if (!res) return 0;
    memset(res, 0, sizeof(*res));
    res->pale_qx = res->pale_qz = -1;
    res->swamp_qx = res->swamp_qz = -1;
    res->flower_qx = res->flower_qz = -1;
    if (w <= 0 || h <= 0) return 0;
    pale = (unsigned char *)malloc((size_t)w * h);
    swamp = (unsigned char *)malloc((size_t)w * h);
    flower = (unsigned char *)malloc((size_t)w * h);
    if (!pale || !swamp || !flower) {
        free(pale); free(swamp); free(flower);
        return 0;
    }
    for (z = 0; z < h; z++) {
        for (x = 0; x < w; x++) {
            double e, hh;
            size_t i = (size_t)z * w + x;
            pale[i] = swamp[i] = flower[i] = 0;
            climate_sample(c, (double)(qx + x), (double)(qz + z), shift, &e, &hh);
            if ((e < 0.05 && hh > 0.30) || e > 0.55 || hh < -0.35) {
                double t, cont, ww;
                climate_sample3(c, (double)(qx + x), (double)(qz + z),
                                shift, &t, &cont, &ww);
                if (e < 0.05 && hh > 0.30 &&
                    t >= -0.15 && t <= 0.20 &&
                    fabs(ww) >= 0.40 && fabs(ww) <= 0.9334 &&
                    cont > 0.30)
                    pale[i] = 1;
                if (e > 0.55 && cont > -0.11 && t < 0.20)
                    swamp[i] = 1;
                if (hh < -0.35 && ww < 0.0 &&
                    t >= -0.15 && t <= 0.20)
                    flower[i] = 1;
            }
        }
    }
    res->pale = find_square(pale, w, h, 2, &px, &pz);
    if (res->pale) { res->pale_qx = qx + px; res->pale_qz = qz + pz; }
    res->swamp = find_square(swamp, w, h, 2, &px, &pz);
    if (res->swamp) { res->swamp_qx = qx + px; res->swamp_qz = qz + pz; }
    res->flower = find_square(flower, w, h, 8, &px, &pz);
    if (res->flower) { res->flower_qx = qx + px; res->flower_qz = qz + pz; }
    free(pale); free(swamp); free(flower);
    return 1;
}
