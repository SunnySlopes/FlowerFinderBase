#include "parallel.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace {

thread_local int t_in_parallel = 0;

} // namespace

extern "C" int cts_cpu_count(void)
{
    const unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? static_cast<int>(n) : 1;
}

extern "C" int cts_in_parallel(void)
{
    return t_in_parallel;
}

extern "C" void cts_parallel_for(int count, int threads, int dynamic, CtsTaskFn fn, void *ctx)
{
    if (count <= 0 || fn == nullptr)
        return;

    int workers = threads > 0 ? threads : 1;
    if (workers > count)
        workers = count;

    /* Nested regions stay serial: the outer workers already own the cores. */
    if (workers <= 1 || t_in_parallel > 0) {
        for (int i = 0; i < count; ++i)
            fn(ctx, i);
        return;
    }

    std::atomic<int> next{0};
    const int chunk = dynamic ? 1 : (count + workers - 1) / workers;

    auto body = [&]() {
        ++t_in_parallel;
        for (;;) {
            const int start = next.fetch_add(chunk, std::memory_order_relaxed);
            if (start >= count)
                break;
            const int stop = dynamic ? start + 1 : std::min(start + chunk, count);
            for (int i = start; i < stop; ++i)
                fn(ctx, i);
            if (dynamic)
                continue;
        }
        --t_in_parallel;
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers - 1));
    try {
        for (int i = 0; i < workers - 1; ++i)
            pool.emplace_back(body);
    } catch (...) {
        /* Thread creation failed: keep whatever started and run the rest here. */
    }
    body();
    for (std::thread &thread : pool) {
        if (thread.joinable())
            thread.join();
    }
}
