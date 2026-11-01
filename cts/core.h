#ifndef CTS_CORE_H
#define CTS_CORE_H

#include <stdint.h>
#include <stdio.h>

#include "generator.h"
#include "biomenoise.h"
#include "noise.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BiomeNoise bn;
    uint64_t seed;
    int large;
} Climate;

typedef struct {
    double e_min, e_max, h_min, h_max;
    int e_min_qx, e_min_qz, e_max_qx, e_max_qz;
    int h_min_qx, h_min_qz, h_max_qx, h_max_qz;
    int pass;
} WindowEval;

typedef struct {
    int qx, qz;          /* window top-left, quart coords */
    WindowEval w;
} Hit;

/* 1.21.5 parameter-box biome check result.  Patch sizes are in 4-block cells;
 * pale/swamp require a 2x2-cell (8x8-block) square, flower forest an
 * 8x8-cell (32x32-block) square. */
typedef struct {
    int pale, swamp, flower;
    int pale_qx, pale_qz;
    int swamp_qx, swamp_qz;
    int flower_qx, flower_qz;
} BiomeResult;

typedef struct {
    double e_lo, e_hi;   /* erosion thresholds */
    double h_lo, h_hi;   /* humidity thresholds */
    int window;          /* block size X, default 320 */
    int window_z;        /* block size Z, default = window */
    int shift;           /* 1 = cubiomes shift on final verification */
    int max_hits;
    int cluster;         /* merge reported hits within this many blocks (0=off) */
    int offsets;         /* exact-check offsets: 1 = base only, 2 = base/+4 quart */
    int quick;           /* 1 = coarse quick reject before the full window eval */
    double qm_e, qm_h;   /* quick-pattern margins (corner/diagonal samples) */
    int l1_step;         /* L1b grid step in quart (8 = 32 blocks; 16 = 64) */
    int l1_boundary;     /* 1 = L1b uses a boundary check at the best L1a window */
    int e_first;         /* 1 = skip the L1b H field when no E window passes */
    int perimeter;       /* 1 = boundary pre-check before the full window */
    int perimeter_step;  /* boundary sampling step in quart (1,2,4,8) */
    double pm_e, pm_h;   /* perimeter pre-check margins */
    int full_verify;     /* 1 = full 4-block verify; 0 = accept the perimeter */
    int no_pale;         /* 1 = coarse E/H only needs swamp hi-E + flower lo-H */
    int threads;         /* explicit OpenMP team size (0 = library default) */
    int max_threads;     /* machine limit; fixes the tile grid independent of threads */
} ScanCfg;

typedef struct {
    double t_fill, t_slide, t_l1, t_exact, t_total;
    int64_t points;
    int64_t windows;
    int64_t cand_l0;
    int64_t cand_l1a;
    int64_t cand_l1;
    int64_t exact_checks;
    int64_t quick_rejects;
    int64_t perimeter_checks;
    int64_t perimeter_rejects;
    int64_t hits;
} ScanStats;

int64_t floor_div4(int64_t v);
double now_sec(void);

int climate_init(Climate *c, uint64_t seed, int large);
void climate_sample(const Climate *c, double qx, double qz, int shift,
                    double *e, double *h);
void climate_sample5(const Climate *c, double qx, double qz, int shift,
                     double *t, double *h, double *cont, double *e, double *w);
void climate_sample3(const Climate *c, double qx, double qz, int shift,
                     double *t, double *cont, double *w);
double climate_t(const Climate *c, double qx, double qz, int shift);
double climate_c(const Climate *c, double qx, double qz, int shift);
double climate_w(const Climate *c, double qx, double qz, int shift);

/* Biome-box scan at 1-quart resolution over a winx x winz rectangle. */
int biome_check_window(const Climate *c, int qx, int qz, int winx, int winz,
                       int shift, BiomeResult *res);
int biome_check_window_real(const Climate *c, const Generator *g, int qx, int qz,
                            int winx, int winz, int y, BiomeResult *res);
/* Hierarchical 8-block condition scan + 4-block actual refine.
 * require_pale: when 0, pale garden is optional (swamp still required). */
int biome_check_window_fast(const Climate *c, const Generator *g, int qx, int qz,
                            int winx, int winz, int y, int require_pale,
                            BiomeResult *res);

/* Cheap 3x3 T/C/W precheck: 0 means no combo can exist in the window.
 * require_pale: when 0, skip pale-garden weirdness / continentalness floors. */
int biome_precheck(const Climate *c, int qx, int qz, int winx, int winz, int shift,
                   int require_pale);

/* 8-block E/H scan: 0 means there is no pale-garden candidate cell at all,
 * so there can be no 8x8 pale patch. */
int biome_pale_possible(const Climate *c, int qx, int qz, int winx, int winz,
                        int shift);
int biome_pale_actual_possible(const Climate *c, const Generator *g, int qx,
                               int qz, int winx, int winz, int y);

/* Global actual-biome cache, keyed by seed/dim/y/x/z.  Thread-safe and
 * lock-free; callers may read it from parallel verification workers. */
int biome_at_cached(const Generator *g, int x, int y, int z);
int *biome_map(const Generator *g, int x, int z, int w, int h, int y, int scale);
/* Debug counters for the calling thread only (the cache is shared, but the
 * counters are thread-local to keep the hot path contention-free). */
void biome_cache_stats(uint64_t *hits, uint64_t *misses);
void climate_low_h(const Climate *c, double qx, double qz,
                   double *e_low, double *h);
double climate_e(const Climate *c, double qx, double qz, int shift);
double climate_e_low(const Climate *c, double qx, double qz);
double climate_e_high(const Climate *c, double qx, double qz);
double climate_h(const Climate *c, double qx, double qz);

/* Exact min/max of E and H over an 80x80 quart window (320 blocks). */
void window_eval(const Climate *c, int qx, int qz, int size_quart, int shift,
                 WindowEval *w);
void window_eval_rect(const Climate *c, int qx, int qz, int winx, int winz,
                      int shift, WindowEval *w);
void window_eval_perimeter_rect(const Climate *c, int qx, int qz,
                                int winx, int winz, int step, int shift,
                                WindowEval *w);

/* Boundary-only min/max: 98%+ of observed window extremes are within 4 blocks
 * of an edge.  ~4*size samples instead of size*size (20x cheaper for 320). */
void window_eval_perimeter(const Climate *c, int qx, int qz, int size_quart,
                           int shift, WindowEval *w);
void window_eval_perimeter_step(const Climate *c, int qx, int qz, int size_quart,
                                int step, int shift, WindowEval *w);


/* Multigrid: 256-block low-octave L0 -> 32-block full-field L1 -> 4-block exact. */
int screen_area(const Climate *c, int64_t bx0, int64_t bz0, int64_t bx1, int64_t bz1,
                const ScanCfg *cfg, int64_t step_l0, double m_e, double m_h,
                double bg, Hit **hits, int *nhits, ScanStats *st);

/* Single-seed parallelism: split the area into overlapping tiles, screen
 * them in parallel, then merge/cluster.  threads <= 0 uses OMP max. */
int screen_area_parallel(const Climate *c, int64_t bx0, int64_t bz0,
                         int64_t bx1, int64_t bz1, const ScanCfg *cfg,
                         int64_t step_l0, double m_e, double m_h, double bg,
                         int threads, Hit **hits, int *nhits, ScanStats *st);

/* One shared coarse E/H field, evaluated for both 640x320 orientations.
 * Returns hits_x (long edge X) and hits_z (long edge Z) separately. */
int screen_area_rect_both(const Climate *c, int64_t bx0, int64_t bz0,
                          int64_t bx1, int64_t bz1, const ScanCfg *cfg,
                          int64_t step_l0, double m_e, double m_h,
                          Hit **hits_x, int *nhits_x,
                          Hit **hits_z, int *nhits_z, ScanStats *st);
int screen_area_rect_both_parallel(const Climate *c, int64_t bx0, int64_t bz0,
                                   int64_t bx1, int64_t bz1, const ScanCfg *cfg,
                                   int64_t step_l0, double m_e, double m_h,
                                   int threads,
                                   Hit **hits_x, int *nhits_x,
                                   Hit **hits_z, int *nhits_z, ScanStats *st);

/* Optional search progress. current/total are plain ints updated atomically. */
void cts_bind_progress(int *current, int *total);

/* Optional auxiliary sub-phase progress:
 *   phase 0 (fast mode only) - coarse E/H field rows filled
 *   phase 2                  - candidate hits merged/clustered
 * Kept separate from the phase progress so the phase-1 total stays readable
 * after the sub-phase.  Bind NULL to disable. */
void cts_bind_aux_progress(int *current, int *total);

/* Optional sub-stage signal. The engine stores 1 while the final merge/cluster
 * tail (after the scan rows finished) is running and 0 otherwise, so hosts can
 * label that tail separately. Pass NULL to unbind. */
void cts_bind_stage(int *stage);

/* Optional cooperative control.  stopped() is a cheap non-blocking check;
 * checkpoint() blocks while paused and returns nonzero when the search must
 * abort (stopped, including a stop issued during a pause).  Both callbacks are
 * called from worker threads and must be thread-safe.  Pass NULL to unbind. */
typedef int (*CtsControlFn)(void *user);
void cts_bind_control(CtsControlFn stopped, CtsControlFn checkpoint, void *user);

#ifdef __cplusplus
}
#endif

#endif
