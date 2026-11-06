#include "com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <stdlib.h>
#include <thread>
#include <vector>

#include "FlowerFarmSearch.h"
#include "cubiomes/biomes.h"
#include "cubiomes/util.h"

/**
 * Library load hook: configure libgomp before it creates its worker pool.
 *
 * Between parallel regions the libgomp workers spin (GOMP_SPINCOUNT defaults
 * to a high value).  With more threads than physical cores that idle spinning
 * steals cycles from the serial parts of the search (sort / de-dupe / JNI
 * marshalling) and from the UI, which showed up as a 20%+ slowdown at
 * 32 threads.  Disabling the spin and asking for passive waiting costs nothing
 * here because this search enters only a handful of long parallel regions.
 *
 * Existing user settings are never overwritten.
 */
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *, void *)
{
#ifdef _WIN32
    if (std::getenv("GOMP_SPINCOUNT") == nullptr)
        _putenv_s("GOMP_SPINCOUNT", "0");
    if (std::getenv("OMP_WAIT_POLICY") == nullptr)
        _putenv_s("OMP_WAIT_POLICY", "passive");
#else
    if (std::getenv("GOMP_SPINCOUNT") == nullptr)
        setenv("GOMP_SPINCOUNT", "0", 1);
    if (std::getenv("OMP_WAIT_POLICY") == nullptr)
        setenv("OMP_WAIT_POLICY", "passive", 1);
#endif
    return JNI_VERSION_1_8;
}

struct Progress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int phase{0};          // 0 = preparation, 1 = screen, 2 = merge/cluster tail, 3 = verify
    std::atomic_int overallCurrent{0};
    std::atomic_int overallTotal{0};
    std::atomic_int stage{0};          // 1 while the post-screen merge/cluster tail runs
    std::atomic_int auxCurrent{0};   // phase-2 progress, in candidate hits
    std::atomic_int auxTotal{0};
    std::atomic_int tileCurrent{0};  // completed tiles of a tiled search
    std::atomic_int tileTotal{0};    // total tiles (0 = single-block search)
    std::atomic_bool deferPause{false};
    std::atomic_bool try_pause{false};
    std::atomic_bool try_stop{false};
};

static std::mutex searchMutex;
static Progress progress{};

static void resetProgressState()
{
    progress.current.store(0);
    progress.total.store(0);
    progress.phase.store(0);
    progress.overallCurrent.store(0);
    progress.overallTotal.store(0);
    progress.stage.store(0);
    progress.auxCurrent.store(0);
    progress.auxTotal.store(0);
    progress.tileCurrent.store(0);
    progress.tileTotal.store(0);
    progress.deferPause.store(false);
    progress.try_pause.store(false);
    progress.try_stop.store(false);
}

JNIEXPORT jobjectArray JNICALL Java_com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge_flowerSearch
  (JNIEnv *env, jclass, jlong seed, jint minX, jint minZ, jint maxX, jint maxZ,
   jint longSide, jint shortSide, jint squareOnly, jint frameStep, jint numThreads,
   jint bothWildflowers, jint basaltDeltas, jint orientation, jint flowerCenterMargin,
   jstring mcVersion)
{
    std::lock_guard<std::mutex> guard(searchMutex);
    int mc = MC_NEWEST;
    if (mcVersion != nullptr)
    {
        const char *mcStr = env->GetStringUTFChars(mcVersion, nullptr);
        if (mcStr != nullptr)
        {
            int parsed = str2mc(mcStr);
            if (parsed != MC_UNDEF)
                mc = parsed;
            env->ReleaseStringUTFChars(mcVersion, mcStr);
        }
    }
    resetProgressState();
    FlowerSearchControl control{
        &progress.current, &progress.total, &progress.phase,
        &progress.try_pause, &progress.try_stop,
        &progress.current, &progress.total,
        &progress.overallCurrent, &progress.overallTotal,
        &progress.stage,
        &progress.auxCurrent, &progress.auxTotal,
        &progress.tileCurrent, &progress.tileTotal,
        &progress.deferPause
    };
    unsigned hw = std::thread::hardware_concurrency();
    int maxThreads = hw > 0 ? (int) hw : 1;
    int threads = numThreads > 0 ? (int) numThreads : maxThreads;
    if (threads < 1)
        threads = 1;
    if (threads > maxThreads)
        threads = maxThreads;
    std::vector<std::string> lines;
    FlowerSearchOptions options{};
    options.requireBothWildflowers = bothWildflowers != 0;
    options.requireBasaltDeltas = basaltDeltas != 0;
    options.orientation = orientation;
    options.flowerCenterMargin = flowerCenterMargin > 0 ? (int) flowerCenterMargin : 0;
    try {
        lines = runFlowerFarmSearch((uint64_t) seed, mc, minX, minZ, maxX, maxZ,
            longSide, shortSide, squareOnly != 0, frameStep, threads, control, options);
    } catch (const std::bad_alloc &) {
        resetProgressState();
        return nullptr;
    } catch (const std::exception &) {
        resetProgressState();
        return nullptr;
    }
    if (progress.try_stop.load())
    {
        resetProgressState();
        return nullptr;
    }
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize) lines.size(), stringClass, nullptr);
    if (!arr)
    {
        resetProgressState();
        return nullptr;
    }
    for (jsize i = 0; i < (jsize) lines.size(); i++)
    {
        jstring s = env->NewStringUTF(lines[(size_t) i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    resetProgressState();
    return arr;
}

JNIEXPORT jintArray JNICALL Java_com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge_getSearchProgress
  (JNIEnv *env, jclass)
{
    int tot = progress.total.load();
    if (tot <= 0)
        return nullptr;

    int phase = progress.phase.load();
    /* The merge/cluster tail after the climate screen is phase 2; verification
     * is phase 3, so the reported phases follow the search order. */
    if (phase == 1 && progress.stage.load() != 0)
        phase = 2;
    int cur = progress.current.load();
    /* The engine may tick slightly past a preset total; never report past 100%. */
    if (tot > 0 && cur > tot)
        cur = tot;
    int status = 1;
    if (phase == 3)
        status = cur >= tot ? 2 : 1;
    int overallCur = progress.overallCurrent.load();
    int overallTot = progress.overallTotal.load();
    if (overallTot <= 0) {
        overallCur = cur;
        overallTot = tot;
    } else if (overallCur > overallTot) {
        overallCur = overallTot;
    }

    int auxCur = progress.auxCurrent.load();
    int auxTot = progress.auxTotal.load();
    if (auxTot > 0 && auxCur > auxTot)
        auxCur = auxTot;

    /* Tile progress of a tiled (large-range) search; tileTotal == 0 means the
     * search ran as a single block. */
    int tileTot = progress.tileTotal.load();
    int tileCur = progress.tileCurrent.load();
    if (tileTot > 0) {
        if (tileCur > tileTot)
            tileCur = tileTot;
    } else {
        tileCur = 0;
    }

    /* Indices 10/11 carry the phase-2 merge progress (candidate hits); 12/13/14
     * carry tile progress.  Older hosts read only the first 10 entries. */
    jint data[15] = {
        cur, tot, phase, status,
        progress.try_pause.load() ? 1 : 0,
        progress.try_stop.load() ? 1 : 0,
        cur, tot,
        overallCur, overallTot,
        auxCur, auxTot,
        tileTot > 0 ? 1 : 0, tileCur, tileTot
    };
    jintArray arr = env->NewIntArray(15);
    if (!arr)
        return nullptr;
    env->SetIntArrayRegion(arr, 0, 15, data);
    return arr;
}

JNIEXPORT jboolean JNICALL Java_com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge_pause(JNIEnv *, jclass)
{
    progress.try_pause.store(true);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge_resume(JNIEnv *, jclass)
{
    progress.try_pause.store(false);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge_stop(JNIEnv *, jclass)
{
    progress.try_stop.store(true);
    return JNI_TRUE;
}
