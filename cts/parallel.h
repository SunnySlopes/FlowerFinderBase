#ifndef CTS_PARALLEL_H
#define CTS_PARALLEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal, dependency-free replacement for the OpenMP pragmas used by the CTS
 * engine.  Backed by std::thread (parallel.cpp) and therefore available on both
 * the MSYS2 native build (winpthreads) and Emscripten -pthread. */

typedef void (*CtsTaskFn)(void *ctx, int index);

/* Machine thread count (>= 1). */
int cts_cpu_count(void);

/* Runs fn(ctx, i) for i in [0, count).
 *
 * threads <= 1, count <= 1, or a call made from inside another
 * cts_parallel_for() worker (nested parallelism) run serially on the caller.
 * dynamic != 0 mirrors OpenMP's schedule(dynamic,1); otherwise the indices are
 * split into one contiguous chunk per worker. */
void cts_parallel_for(int count, int threads, int dynamic, CtsTaskFn fn, void *ctx);

/* Nonzero while the calling thread is inside a cts_parallel_for() worker. */
int cts_in_parallel(void);

#ifdef __cplusplus
}
#endif

#endif /* CTS_PARALLEL_H */
