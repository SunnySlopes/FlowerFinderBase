#include "FlowerFarmSearch.h"

#include "core.h"
#include "cubiomes/biomes.h"
#include "cubiomes/noise.h"
#include "cubiomes/util.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

/* CTS biomes defaults from cts 1.0.0 cmd_biomes. */
constexpr int64_t kStep = 256;
constexpr double kMarginE = 0.08;
constexpr double kMarginH = 0.12;
constexpr double kBg = 0.09;
constexpr int kBiomeY = 256;
constexpr int kNetherY = 64;
constexpr int kPaleDist = 64;
constexpr int kFlowerAreaMin = 2000;

int stopRequested(void *user)
{
    auto *c = static_cast<FlowerSearchControl *>(user);
    return (c && c->stopReq && c->stopReq->load()) ? 1 : 0;
}

/* Blocks while paused; returns nonzero when the caller must abort. */
int waitForPause(void *user)
{
    auto *c = static_cast<FlowerSearchControl *>(user);
    if (!c)
        return 0;
    while (c->pauseReq && c->pauseReq->load())
    {
        if (c->stopReq && c->stopReq->load())
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return (c->stopReq && c->stopReq->load()) ? 1 : 0;
}

bool pauseOk(FlowerSearchControl &c)
{
    return waitForPause(&c) == 0;
}

/* Binds the C engine to the C++ control object for the lifetime of a search. */
struct CtsBinding {
    explicit CtsBinding(FlowerSearchControl &c) : control(c)
    {
        cts_bind_control(&stopRequested, &waitForPause, &control);
        cts_bind_stage(control.stage ? reinterpret_cast<int *>(control.stage) : nullptr);
    }
    ~CtsBinding()
    {
        cts_bind_control(nullptr, nullptr, nullptr);
        cts_bind_stage(nullptr);
        cts_bind_progress(nullptr, nullptr);
        cts_bind_aux_progress(nullptr, nullptr);
    }
    FlowerSearchControl &control;
};

void storeInt(std::atomic_int *p, int value)
{
    if (p)
        p->store(value);
}

int loadInt(std::atomic_int *p)
{
    return p ? p->load() : 0;
}

void setPhase(FlowerSearchControl &c, int phase, int total)
{
    storeInt(c.phase, phase);
    storeInt(c.total, total);
    storeInt(c.current, 0);
    if (c.phaseTotal && c.phaseTotal != c.total)
        storeInt(c.phaseTotal, total);
    if (c.phaseCurrent && c.phaseCurrent != c.current)
        storeInt(c.phaseCurrent, 0);
}

void setPhaseCurrent(FlowerSearchControl &c, int current)
{
    storeInt(c.current, current);
    if (c.phaseCurrent && c.phaseCurrent != c.current)
        storeInt(c.phaseCurrent, current);
}

void setOverall(FlowerSearchControl &c, int current, int total)
{
    storeInt(c.overallCurrent, current);
    storeInt(c.overallTotal, total);
}

int alignDown(int value, int multiple, int minimum)
{
    if (value < minimum)
        return 0;
    value -= value % multiple;
    return value >= minimum ? value : 0;
}

ScanCfg makeCfg(int windowX, int windowZ, int fullVerify, int noPale, int cluster,
                int threads)
{
    ScanCfg cfg{};
    cfg.e_lo = 0.05;
    cfg.e_hi = 0.55;
    cfg.h_lo = -0.35;
    cfg.h_hi = 0.30;
    cfg.window = windowX;
    cfg.window_z = windowZ;
    cfg.shift = 1;
    cfg.max_hits = 50000;
    /* Climate-stage cluster keeps phase-1 hit lists small (overlap is extreme without it).
     * Verified rows are clustered again with cherry / origin preference. */
    cfg.cluster = cluster;
    cfg.offsets = 2;
    cfg.quick = 1;
    cfg.qm_e = 0.02;
    cfg.qm_h = 0.0;
    cfg.l1_step = 8;
    cfg.l1_boundary = 0;
    cfg.e_first = 1;
    cfg.perimeter = 0;
    /* Fast mode with a rectangular window samples the border every 16 blocks
     * instead of 8: the perimeter is the dominant cost for large windows
     * (960x640), and the coarser border only drops a few marginal windows.
     * Square windows keep the original 8-block border. */
    cfg.perimeter_step = (fullVerify || windowX == windowZ) ? 2 : 4;
    cfg.pm_e = 0.02;
    cfg.pm_h = 0.02;
    cfg.full_verify = fullVerify ? 1 : 0;
    cfg.no_pale = noPale ? 1 : 0;
    cfg.threads = threads > 0 ? threads : 1;
    const unsigned hw = std::thread::hardware_concurrency();
    cfg.max_threads = hw > 0 ? (int) hw : 1;
    return cfg;
}

int64_t floorDiv8(int64_t v)
{
    return v >= 0 ? v / 8 : -((-v + 7) / 8);
}

/* Internal row: public fields plus search-only ranking keys. */
struct OutRow : FlowerRow {
    int score = 0;
    int hasPinkPetals = 0;
    int order = 0;
    int qx = 0;
    int qz = 0;
};

/* GUI: 1.18~1.21.3 → MC_1_21_3; 1.21.4+ → newest. */
struct VersionRules {
    bool requirePale = true;
    bool allowBirchMeadow = true;
    bool allowCherry = true;
    bool reportPaleArea = true;
    bool pinkPetalsVersionNote = false;
    /* Phase-1 candidate clustering (blocks, Chebyshev on window offsets).
     * This is only a pre-dedupe for the candidate lists; verified rows are
     * de-duplicated by their actual window overlap instead. */
    int clusterBlocks = 80;
};

VersionRules rulesForMc(int mc)
{
    VersionRules r;
    if (mc < MC_1_21_4)
    {
        r.requirePale = false;
        r.allowBirchMeadow = false;
        r.reportPaleArea = false;
        r.allowCherry = true;
        r.pinkPetalsVersionNote = true;
        r.clusterBlocks = 160;
    }
    return r;
}

/** Aggregate bonus labels: birch/meadow → wildflowers, cherry → pink_petals. */
std::string largestWoodBonus(int birch, int oldBirch, int meadow, int cherry,
                             const VersionRules &rules)
{
    std::string bonus;
    if (rules.allowBirchMeadow && (birch > 0 || oldBirch > 0 || meadow > 0))
        bonus = "wildflowers";
    if (rules.allowCherry && cherry > 0)
    {
        if (!bonus.empty())
            bonus += '+';
        bonus += rules.pinkPetalsVersionNote ? "pink_petals(1.19.4+)" : "pink_petals";
    }
    return bonus;
}

/** Best nether biome under the overworld window (rank: basalt → … → warped). */
std::string netherBiomesInWindow(const Generator &nether, int wx, int wz,
                                 int winxBlocks, int winzBlocks, int mc)
{
    static const int kRank[] = {
        basalt_deltas, nether_wastes, crimson_forest, soul_sand_valley, warped_forest
    };
    bool seen[256] = {};
    const int nMinX = (int) floorDiv8(wx);
    const int nMaxX = (int) floorDiv8((int64_t) wx + winxBlocks - 1);
    const int nMinZ = (int) floorDiv8(wz);
    const int nMaxZ = (int) floorDiv8((int64_t) wz + winzBlocks - 1);
    const int spanX = nMaxX - nMinX + 1;
    const int spanZ = nMaxZ - nMinZ + 1;
    /* Dense enough for ranking; stride grows for large windows. */
    const int step = (spanX > 64 || spanZ > 64) ? 2 : 1;
    for (int nz = nMinZ; nz <= nMaxZ; nz += step)
    {
        for (int nx = nMinX; nx <= nMaxX; nx += step)
        {
            const int id = biome_at_cached(&nether, nx, kNetherY, nz);
            if (id >= 0 && id < 256)
                seen[id] = true;
        }
    }
    /* Corners in case stride skipped them. */
    const int corners[4][2] = {
        {nMinX, nMinZ}, {nMaxX, nMinZ}, {nMinX, nMaxZ}, {nMaxX, nMaxZ}
    };
    for (const auto &c : corners)
    {
        const int id = biome_at_cached(&nether, c[0], kNetherY, c[1]);
        if (id >= 0 && id < 256)
            seen[id] = true;
    }
    for (int id : kRank)
    {
        if (!seen[id])
            continue;
        const char *name = biome2str(mc, id);
        if (name)
            return name;
    }
    return "unknown";
}

int64_t originDist2(const OutRow &r)
{
    return (int64_t) r.cx * r.cx + (int64_t) r.cz * r.cz;
}

int floorDivInt(int value, int divisor)
{
    const int q = value / divisor;
    const int r = value % divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

uint64_t windowCellKey(int gx, int gz)
{
    return ((uint64_t) (uint32_t) gx << 32) | (uint64_t) (uint32_t) gz;
}

/**
 * De-duplicates verified rows by window overlap: a row is kept only when its
 * sideX × sideZ window does not intersect an already-kept window. `rows` must
 * be sorted best-first, so the preferred window wins (cherry / nearer origin).
 * A spatial hash keeps this O(n) for the sparse verified-row lists.
 */
bool dedupeOverlappingWindows(std::vector<OutRow> &rows, FlowerSearchControl &control)
{
    if (rows.size() <= 1)
        return true;
    /* Window sides are capped at 1024 blocks (MAX_WINDOW), so a window spans at
     * most two cells per axis. */
    constexpr int kCell = 1024;
    std::unordered_map<uint64_t, std::vector<int>> grid;
    std::vector<OutRow> kept;
    kept.reserve(rows.size());
    for (size_t n = 0; n < rows.size(); n++)
    {
        if ((n & 63) == 0 && !pauseOk(control))
            return false;
        const OutRow &h = rows[n];
        const int sideX = std::max(1, h.sideX);
        const int sideZ = std::max(1, h.sideZ);
        const int left = h.cx - sideX / 2;
        const int top = h.cz - sideZ / 2;
        const int gx0 = floorDivInt(left, kCell);
        const int gx1 = floorDivInt(left + sideX - 1, kCell);
        const int gz0 = floorDivInt(top, kCell);
        const int gz1 = floorDivInt(top + sideZ - 1, kCell);
        bool overlaps = false;
        for (int gz = gz0; gz <= gz1 && !overlaps; gz++)
        {
            for (int gx = gx0; gx <= gx1 && !overlaps; gx++)
            {
                const auto it = grid.find(windowCellKey(gx, gz));
                if (it == grid.end())
                    continue;
                for (int index : it->second)
                {
                    const OutRow &k = kept[(size_t) index];
                    /* Half-open rectangles: touching edges do not overlap. */
                    if (std::abs(h.cx - k.cx) * 2 < h.sideX + k.sideX &&
                        std::abs(h.cz - k.cz) * 2 < h.sideZ + k.sideZ)
                    {
                        overlaps = true;
                        break;
                    }
                }
            }
        }
        if (overlaps)
            continue;
        const int index = (int) kept.size();
        for (int gz = gz0; gz <= gz1; gz++)
            for (int gx = gx0; gx <= gx1; gx++)
                grid[windowCellKey(gx, gz)].push_back(index);
        kept.push_back(h);
    }
    rows.swap(kept);
    return true;
}

/** Best-first order shared by de-duplication and the final result list. */
bool rowPreferred(const OutRow &a, const OutRow &b)
{
    if (a.hasPinkPetals != b.hasPinkPetals)
        return a.hasPinkPetals > b.hasPinkPetals;
    const int64_t da = originDist2(a), db = originDist2(b);
    if (da != db)
        return da < db;
    if (a.qx != b.qx)
        return a.qx < b.qx;
    if (a.qz != b.qz)
        return a.qz < b.qz;
    return a.order < b.order;
}

std::string formatRow(const FlowerRow &h, const VersionRules &rules)
{
    std::string line = "/tp " + std::to_string(h.cx) + " 64 " + std::to_string(h.cz) + " side=";
    if (h.sideX == h.sideZ)
        line += std::to_string(h.sideX);
    else
        line += std::to_string(h.sideX) + "x" + std::to_string(h.sideZ);
    line += " flowerArea=" + std::to_string(h.flowerArea);
    if (rules.reportPaleArea)
        line += " paleArea=" + std::to_string(h.paleArea);
    line += " swampArea=" + std::to_string(h.swampArea);
    line += " nether=" + h.nether;
    if (!h.bonus.empty())
        line += " bonus=" + h.bonus;
    return line;
}

struct FlowerNoise {
    DoublePerlinNoise dpn{};
    PerlinNoise oct[4]{};
};

/**
 * Reusable per-thread buffers for considerHit().  Verification runs in
 * parallel over candidates, so keeping these vectors out of the per-candidate
 * heap churn both avoids malloc contention and keeps the sampled window data
 * hot in each worker's caches.  One instance per worker thread.
 */
struct VerifyScratch {
    std::vector<int> flowerX, flowerZ;
    std::vector<int> centreX, centreZ;
    std::vector<int> sampleX, sampleZ;

    VerifyScratch()
    {
        flowerX.reserve(1024);
        flowerZ.reserve(1024);
        centreX.reserve(1024);
        centreZ.reserve(1024);
        sampleX.reserve(512);
        sampleZ.reserve(512);
    }

    void reset()
    {
        flowerX.clear();
        flowerZ.clear();
        centreX.clear();
        centreZ.clear();
        sampleX.clear();
        sampleZ.clear();
    }
};

void initFlowerNoise(FlowerNoise &fn)
{
    Xoroshiro xr;
    xSetSeed(&xr, 2345);
    const double amp[1] = {1.0};
    xDoublePerlinInit(&fn.dpn, &xr, fn.oct, amp, 0, 1, -1);
}

/** Window centre used by the flower-forest edge-margin filter (block coords, inclusive). */
struct WindowCentre {
    int minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    bool active = false;

    bool contains(int x, int z) const
    {
        return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
    }
};

int flowerId(const FlowerNoise &fn, int x, int y, int z)
{
    const double scale = 0.020833334;
    const double value = sampleDoublePerlin(&fn.dpn, x * scale, y * scale, z * scale);
    double t = (1.0 + value) / 2.0;
    if (t < 0.0)
        t = 0.0;
    else if (t > 0.9999)
        t = 0.9999;
    return (int) (t * 11.0);
}

/** Both gradient ends (dandelion = 0, lily of the valley = 10) within 32 blocks vertically. */
bool flowerExtremes(const FlowerNoise &fn, const std::vector<int> &xs, const std::vector<int> &zs, int &outY)
{
    if (xs.empty())
        return false;
    std::vector<int> dandelion;
    std::vector<int> lily;
    for (int y = 80; y <= 319; y += 2)
    {
        bool hasDandelion = false;
        bool hasLily = false;
        for (size_t i = 0; i < xs.size(); i++)
        {
            const int id = flowerId(fn, xs[i], y, zs[i]);
            if (id == 0)
                hasDandelion = true;
            else if (id == 10)
                hasLily = true;
            if (hasDandelion && hasLily)
                break;
        }
        if (hasDandelion)
            dandelion.push_back(y);
        if (hasLily)
            lily.push_back(y);
    }
    int best = 1 << 30;
    int bestY = 0;
    for (int dy : dandelion)
    {
        for (int ly : lily)
        {
            const int span = std::abs(dy - ly);
            if (span <= 32 && span < best)
            {
                best = span;
                bestY = (int) std::lround((dy + ly) / 2.0);
            }
        }
    }
    if (best > 32)
        return false;
    outY = bestY;
    return true;
}

bool considerHit(const Climate &climate, const Generator &overworld, const Generator &nether,
                 const FlowerNoise &flowers, int mc, const VersionRules &rules, const Hit &hit,
                 int winxBlocks, int winzBlocks, char orient, int order,
                 FlowerSearchControl &control, const FlowerSearchOptions &options,
                 VerifyScratch &scratch, OutRow &row, bool &accepted)
{
    accepted = false;
    if (!pauseOk(control))
        return false;
    /* Orientation filter: horizontal keeps windows with sideX >= sideZ, vertical
     * the opposite. Square windows satisfy both and are always kept. */
    if (options.orientation == FLOWER_ORIENT_HORIZONTAL && winxBlocks < winzBlocks)
        return true;
    if (options.orientation == FLOWER_ORIENT_VERTICAL && winzBlocks < winxBlocks)
        return true;
    const int winx = winxBlocks / 4;
    const int winz = winzBlocks / 4;
    if (!biome_precheck(&climate, hit.qx, hit.qz, winx, winz, 1, rules.requirePale ? 1 : 0))
        return true;
    if (rules.requirePale)
    {
        if (std::abs(hit.w.e_min_qx - hit.w.h_max_qx) * 4 > kPaleDist ||
            std::abs(hit.w.e_min_qz - hit.w.h_max_qz) * 4 > kPaleDist)
            return true;
        if (!biome_pale_actual_possible(&climate, &overworld, hit.qx, hit.qz, winx, winz, kBiomeY))
            return true;
    }

    BiomeResult br{};
    biome_check_window_fast(&climate, &overworld, hit.qx, hit.qz, winx, winz, kBiomeY,
                            rules.requirePale ? 1 : 0, &br);
    if (rules.requirePale)
    {
        if (!(br.pale && br.swamp))
            return true;
    }
    else if (!br.swamp)
    {
        return true;
    }
    if (!pauseOk(control))
        return false;

    const int wx = hit.qx * 4;
    const int wz = hit.qz * 4;
    scratch.reset();
    std::vector<int> &flowerX = scratch.flowerX;
    std::vector<int> &flowerZ = scratch.flowerZ;
    int paleCells = 0, swampCells = 0;
    int birchCells = 0, oldCells = 0, meadowCells = 0, cherryCells = 0;
    for (int qz = 0; qz < winz; qz++)
    {
        for (int qx = 0; qx < winx; qx++)
        {
            const int bx = wx + qx * 4;
            const int bz = wz + qz * 4;
            const int id = biome_at_cached(&overworld, bx, kBiomeY, bz);
            if (id == flower_forest)
            {
                flowerX.push_back(bx + 2);
                flowerZ.push_back(bz + 2);
            }
            else if (id == pale_garden)
                paleCells++;
            else if (id == swamp)
                swampCells++;
            else if (id == birch_forest)
                birchCells++;
            else if (id == old_growth_birch_forest)
                oldCells++;
            else if (id == meadow)
                meadowCells++;
            else if (id == cherry_grove)
                cherryCells++;
        }
    }
    const int flowerArea = (int) flowerX.size() * 16;
    if (flowerArea <= kFlowerAreaMin)
        return true;
    /* Flower-forest edge margin: when set, only flower-noise columns at least
     * `flowerCenterMargin` blocks inside every window edge may provide the two
     * gradient ends, so both ends are required to fall inside the window centre. */
    WindowCentre centre;
    if (options.flowerCenterMargin > 0)
    {
        const int margin = options.flowerCenterMargin;
        if (winxBlocks <= 2 * margin || winzBlocks <= 2 * margin)
            return true; /* empty centre: no window can pass */
        centre.active = true;
        centre.minX = wx + margin;
        centre.maxX = wx + winxBlocks - 1 - margin;
        centre.minZ = wz + margin;
        centre.maxZ = wz + winzBlocks - 1 - margin;
    }
    std::vector<int> &centreX = scratch.centreX;
    std::vector<int> &centreZ = scratch.centreZ;
    for (size_t i = 0; i < flowerX.size(); i++)
    {
        if (centre.active && !centre.contains(flowerX[i], flowerZ[i]))
            continue;
        centreX.push_back(flowerX[i]);
        centreZ.push_back(flowerZ[i]);
    }
    const int cap = 512;
    const int stride = std::max(1, ((int) centreX.size() + cap - 1) / cap);
    std::vector<int> &sampleX = scratch.sampleX;
    std::vector<int> &sampleZ = scratch.sampleZ;
    for (size_t i = 0; i < centreX.size(); i += (size_t) stride)
    {
        sampleX.push_back(centreX[i]);
        sampleZ.push_back(centreZ[i]);
    }
    int flowerY = 0;
    if (!pauseOk(control))
        return false;
    if (!flowerExtremes(flowers, sampleX, sampleZ, flowerY))
        return true;

    const int cx = wx + winxBlocks / 2;
    const int cz = wz + winzBlocks / 2;
    const bool woodBonus = rules.allowBirchMeadow && (birchCells || oldCells || meadowCells);
    const bool cherryBonus = rules.allowCherry && cherryCells > 0;
    /* "Both wildflowers" filter: the window must contain the wildflower biome
     * bonus and the cherry (pink petals) bonus at the same time. */
    if (options.requireBothWildflowers && !(woodBonus && cherryBonus))
        return true;
    if (!pauseOk(control))
        return false;
    const std::string netherList =
        netherBiomesInWindow(nether, wx, wz, winxBlocks, winzBlocks, mc);
    const bool basalt = netherList.find("basalt_deltas") != std::string::npos;
    /* "Basalt deltas" filter: the nether counterpart window must contain it.
     * basalt_deltas is first in the ranking, so it heads the reported list. */
    if (options.requireBasaltDeltas && !basalt)
        return true;
    row.score = (basalt ? 1 : 0) + (woodBonus ? 1 : 0) + (cherryBonus ? 1 : 0);
    row.hasPinkPetals = cherryBonus ? 1 : 0;
    row.order = order;
    row.qx = hit.qx;
    row.qz = hit.qz;
    row.sideX = winxBlocks;
    row.sideZ = winzBlocks;
    row.cx = cx;
    row.cz = cz;
    row.paleX = br.pale_qx * 4;
    row.paleZ = br.pale_qz * 4;
    row.swampX = br.swamp_qx * 4;
    row.swampZ = br.swamp_qz * 4;
    row.flowerX = flowerX.front() - 2;
    row.flowerZ = flowerZ.front() - 2;
    row.flowerY = flowerY;
    row.flowerArea = flowerArea;
    row.paleArea = paleCells * 16;
    row.swampArea = swampCells * 16;
    row.orient = orient;
    row.bonus = largestWoodBonus(birchCells, oldCells, meadowCells, cherryCells, rules);
    row.nether = netherList;
    accepted = true;
    return true;
}

} // namespace

std::vector<FlowerRow> runFlowerFarmSearchRows(
    uint64_t seed,
    int mc,
    int minX,
    int minZ,
    int maxX,
    int maxZ,
    int longSide,
    int shortSide,
    bool squareOnly,
    int frameStep,
    int threads,
    FlowerSearchControl &control,
    const FlowerSearchOptions &options)
{
    std::vector<FlowerRow> result;
    if (mc == MC_UNDEF)
        mc = MC_NEWEST;
    if (maxX < minX || maxZ < minZ)
        return result;

    /* "threads <= 0" or anything above the machine limit means "use all cores". */
    const unsigned hwThreads = std::thread::hardware_concurrency();
    const int maxThreads = hwThreads > 0 ? (int) hwThreads : 1;
    if (threads <= 0 || threads > maxThreads)
        threads = maxThreads;

    const VersionRules rules = rulesForMc(mc);

    const bool square = squareOnly || shortSide == longSide;
    const int windowX = alignDown(longSide, square ? 32 : 8, square ? 64 : 8);
    const int windowZ = square ? windowX : alignDown(shortSide, 8, 8);
    if (windowX <= 0 || windowZ <= 0)
        return result;

    CtsBinding binding(control);
    setPhase(control, 0, 1);
    setOverall(control, 0, 0);
    if (!pauseOk(control))
        return result;

    Climate climate{};
    climate_init(&climate, seed, 0);
    if (mc != MC_NEWEST)
    {
        initBiomeNoise(&climate.bn, mc);
        setBiomeSeed(&climate.bn, seed, 0);
    }

    ScanCfg cfg = makeCfg(windowX, windowZ, frameStep <= 4, rules.requirePale ? 0 : 1,
                          rules.clusterBlocks, threads);
    const int64_t x0 = minX;
    const int64_t z0 = minZ;
    const int64_t x1 = (int64_t) maxX + 1;
    const int64_t z1 = (int64_t) maxZ + 1;

    setPhase(control, 1, 1);
    setOverall(control, 0, 0);
    cts_bind_progress(reinterpret_cast<int *>(control.current), reinterpret_cast<int *>(control.total));
    /* The merge/cluster tail reports candidate hits on its own counters so the
     * phase-1 total above stays intact for the overall progress. */
    cts_bind_aux_progress(reinterpret_cast<int *>(control.auxCurrent),
                            reinterpret_cast<int *>(control.auxTotal));

    Hit *hits = nullptr;
    Hit *hitsZ = nullptr;
    int nHits = 0;
    int nHitsZ = 0;
    ScanStats stats{};
    const bool dual = windowX != windowZ;
    int screened = 0;
    if (dual)
    {
        screened = screen_area_rect_both_parallel(&climate, x0, z0, x1, z1, &cfg, kStep,
            kMarginE, kMarginH, threads, &hits, &nHits, &hitsZ, &nHitsZ, &stats);
    }
    else
    {
        screened = screen_area_parallel(&climate, x0, z0, x1, z1, &cfg, kStep,
            kMarginE, kMarginH, kBg, threads, &hits, &nHits, &stats);
    }
    cts_bind_progress(nullptr, nullptr);
    cts_bind_aux_progress(nullptr, nullptr);
    if (!screened || !pauseOk(control))
    {
        std::free(hits);
        std::free(hitsZ);
        return result;
    }

    /* Phase 3 (verification) total: one unit per candidate plus one finalize unit
     * (sort / overlap dedupe / copy), so the bar never sits at 100% while the
     * tail still runs. When an orientation is required, the other orientation's
     * candidate list is not verified at all. Phase 2 is the merge/cluster tail
     * that the climate screen reports through `stage` before this point. */
    const int phase1Total = std::max(1, loadInt(control.total));
    const bool useXList = !dual || options.orientation != FLOWER_ORIENT_VERTICAL;
    const bool useZList = dual && options.orientation != FLOWER_ORIENT_HORIZONTAL;
    const int candidateCount = (useXList ? nHits : 0) + (useZList ? nHitsZ : 0);
    const int phase3Total = candidateCount < INT_MAX ? candidateCount + 1 : INT_MAX;
    const int overallTotal = phase1Total > INT_MAX - phase3Total
        ? INT_MAX : phase1Total + phase3Total;
    setPhase(control, 3, phase3Total);
    setOverall(control, phase1Total, overallTotal);

    Generator overworld{};
    Generator nether{};
    setupGenerator(&overworld, mc, 0);
    applySeed(&overworld, DIM_OVERWORLD, seed);
    setupGenerator(&nether, mc, 0);
    applySeed(&nether, DIM_NETHER, seed);

    FlowerNoise flowers;
    initFlowerNoise(flowers);

    /* Phase 3 is the expensive part: every candidate re-samples a full window
     * of biomes plus the flower-noise height extremes.  All inputs here are
     * read-only (Climate / Generator / FlowerNoise), and the only shared state
     * is either atomic (biome cache, progress counters, pause/stop flags) or
     * collected per thread, so candidates can be verified in parallel.
     * Results are merged under one critical section and then sorted with a
     * total order (rowPreferred ends on the unique candidate ordinal), so the
     * final list does not depend on which worker finished first. */
    struct VerifyTask {
        const Hit *hit;
        int winx, winz;
        char orient;
        int order;
    };
    std::vector<VerifyTask> tasks;
    tasks.reserve((size_t) std::min(candidateCount, 65536));
    if (dual)
    {
        const char orientX = windowX >= windowZ ? 'X' : 'Z';
        const char orientZ = orientX == 'X' ? 'Z' : 'X';
        if (useXList)
        {
            for (int i = 0; i < nHits; i++)
                tasks.push_back({&hits[i], windowX, windowZ, orientX, i});
        }
        if (useZList)
        {
            for (int i = 0; i < nHitsZ; i++)
                tasks.push_back({&hitsZ[i], windowZ, windowX, orientZ, nHits + i});
        }
    }
    else
    {
        for (int i = 0; i < nHits; i++)
            tasks.push_back({&hits[i], windowX, windowZ, 0, i});
    }

    std::vector<OutRow> rows;
    rows.reserve((size_t) std::min(candidateCount, 4096));
    const int taskCount = (int) tasks.size();
    const int verifyThreads = std::max(1, std::min(threads, taskCount));
    std::atomic<bool> keep{true};
    std::atomic<int> verified{0};
    if (taskCount > 0)
    {
#pragma omp parallel num_threads(verifyThreads)
        {
            std::vector<OutRow> local;
            local.reserve(64);
            VerifyScratch scratch;
#pragma omp for schedule(dynamic, 1)
            for (int i = 0; i < taskCount; i++)
            {
                if (!keep.load(std::memory_order_relaxed))
                    continue;
                const VerifyTask &task = tasks[(size_t) i];
                OutRow row;
                bool accepted = false;
                if (!considerHit(climate, overworld, nether, flowers, mc, rules, *task.hit,
                                 task.winx, task.winz, task.orient, task.order, control,
                                 options, scratch, row, accepted))
                {
                    keep.store(false, std::memory_order_relaxed);
                    continue;
                }
                if (accepted)
                    local.push_back(std::move(row));
                const int n = verified.fetch_add(1, std::memory_order_relaxed) + 1;
                /* The UI polls at ~100 ms; per-candidate atomic stores from
                 * every worker are pure contention. */
                if ((n & 15) == 0)
                {
                    setPhaseCurrent(control, n);
                    setOverall(control, phase1Total + n, overallTotal);
                }
            }
#pragma omp critical(fff_verify_merge)
            {
                rows.insert(rows.end(), std::make_move_iterator(local.begin()),
                            std::make_move_iterator(local.end()));
            }
        }
    }
    std::free(hits);
    std::free(hitsZ);
    if (!keep.load())
        return result;

    /* Candidates are done; keep one phase-3 unit for the finalize step. */
    setPhaseCurrent(control, candidateCount);
    setOverall(control, phase1Total + candidateCount, overallTotal);

    /* Same rule as GUI Sort: cherry first, then nearer to origin. Sorting before
     * de-duplication lets the preferred window win each overlap. */
    std::sort(rows.begin(), rows.end(), rowPreferred);
    if (!dedupeOverlappingWindows(rows, control))
        return result;
    if (!pauseOk(control))
        return result;

    result.reserve(rows.size());
    for (const OutRow &row : rows)
        result.push_back(static_cast<const FlowerRow &>(row));

    setPhaseCurrent(control, phase3Total);
    setOverall(control, overallTotal, overallTotal);
    return result;
}

std::vector<std::string> runFlowerFarmSearch(
    uint64_t seed,
    int mc,
    int minX,
    int minZ,
    int maxX,
    int maxZ,
    int longSide,
    int shortSide,
    bool squareOnly,
    int frameStep,
    int threads,
    FlowerSearchControl &control,
    const FlowerSearchOptions &options)
{
    const int effectiveMc = mc == MC_UNDEF ? MC_NEWEST : mc;
    const VersionRules rules = rulesForMc(effectiveMc);
    const std::vector<FlowerRow> rows = runFlowerFarmSearchRows(
        seed, mc, minX, minZ, maxX, maxZ, longSide, shortSide, squareOnly,
        frameStep, threads, control, options);
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (const FlowerRow &row : rows)
        lines.push_back(formatRow(row, rules));
    return lines;
}
