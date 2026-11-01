#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Search control shared between the JNI bridge and the native search.
 *
 * current/total always describe the current phase:
 *   0 = preparation, 1 = climate screen, 2 = verification.
 * phaseCurrent/phaseTotal are explicit aliases for hosts that prefer them.
 * overallCurrent/overallTotal cover the whole search; overallTotal is filled
 * once the phase-3 candidate count is known.
 * stage is 1 while the post-screen merge/cluster tail runs and 0 otherwise, so
 * hosts can label that tail as a separate phase. The JNI host reports it as
 * phase 2 and renumbers verification to phase 3, matching the search order.
 *
 * Pointers are optional except current/total/phase while a search runs.
 */
struct FlowerSearchControl {
    std::atomic_int *current = nullptr;
    std::atomic_int *total = nullptr;
    std::atomic_int *phase = nullptr;
    std::atomic_bool *pauseReq = nullptr;
    std::atomic_bool *stopReq = nullptr;
    std::atomic_int *phaseCurrent = nullptr;
    std::atomic_int *phaseTotal = nullptr;
    std::atomic_int *overallCurrent = nullptr;
    std::atomic_int *overallTotal = nullptr;
    std::atomic_int *stage = nullptr;
    /**
     * Auxiliary sub-phase progress: phase-0 coarse-field rows (fast mode only)
     * and phase-2 merged candidate hits.  Separate from current/total so the
     * phase-1 total is still readable after the sub-phase.  Optional; nullptr
     * disables it.
     */
    std::atomic_int *auxCurrent = nullptr;
    std::atomic_int *auxTotal = nullptr;
};

/**
 * One verified flower-farm window, independent of the legacy /tp text format.
 * cx/cz are the window centre in block coordinates; sideX/sideZ are the
 * requested window extents along X/Z (long side last for Z-oriented hits).
 */
struct FlowerRow {
    int cx = 0, cz = 0;
    int sideX = 0, sideZ = 0;
    int flowerArea = 0, paleArea = 0, swampArea = 0;
    int flowerX = 0, flowerY = 0, flowerZ = 0;
    int paleX = 0, paleZ = 0, swampX = 0, swampZ = 0;
    char orient = 0;             // 'X' long edge on X, 'Z' long edge on Z, 0 square
    std::string nether;          // cubiomes English biome key
    std::string bonus;           // wildflowers / pink_petals / wildflowers+pink_petals
};

/** Optional result filters. All default to "off" (keep every verified row). */
struct FlowerSearchOptions {
    /** Keep only windows with birch/meadow (wildflowers) and cherry (pink_petals) both inside. */
    bool requireBothWildflowers = false;
    /** Keep only windows whose nether counterpart contains basalt deltas. */
    bool requireBasaltDeltas = false;
    /** 0 = any shape, 1 = horizontal only (sideX >= sideZ), 2 = vertical only (sideZ >= sideX). */
    int orientation = 0;
    /**
     * Flower-forest edge margin in blocks; 0 = off.
     *
     * When positive, both flower-gradient ends (dandelion / lily of the valley)
     * must lie inside the window centre: the window shrunk by this many blocks
     * on every side. A margin that leaves no centre room rejects every row.
     */
    int flowerCenterMargin = 0;
};

constexpr int FLOWER_ORIENT_ANY = 0;
constexpr int FLOWER_ORIENT_HORIZONTAL = 1;
constexpr int FLOWER_ORIENT_VERTICAL = 2;

/**
 * CTS 1.0.0 climate-transition search (pale garden + swamp + flower forest).
 * Window sizes come from the existing long/short side. frameStep <= 4 enables
 * CTS full-window verification; larger steps keep the CTS boundary screen.
 * Verified rows are de-duplicated by window overlap: overlapping windows are
 * dropped after ranking, so every returned window occupies disjoint blocks.
 * Rows are returned in the same order as the legacy /tp lines.
 */
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
    const FlowerSearchOptions &options = FlowerSearchOptions{});

/**
 * Legacy text API. Each result line is a /tp command. Kept for the Java GUI
 * and existing callers; internally it calls runFlowerFarmSearchRows().
 */
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
    const FlowerSearchOptions &options = FlowerSearchOptions{});
