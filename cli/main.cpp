/*
 * FlowerFinderBase-cli —— 全花农场窗口搜索的独立命令行入口。
 *
 * 与 GUI 共用同一套核心（runFlowerFarmSearch），参数语义与
 * FlowerFarmFinderSearchRunner / FlowerFarmFinderFrame 保持一致，
 * 输出就是 GUI 里展示/导出的那些 /tp 行。
 *
 *   FlowerFinderBase-cli --seed <seed> [options]
 *   FlowerFinderBase-cli --help
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "FlowerFarmSearch.h"

#include "cubiomes/biomes.h"
#include "cubiomes/util.h"

#ifndef FFB_VERSION
#define FFB_VERSION "0.0.0"
#endif

namespace {

/* ---- 与 GUI 一致的默认值 / 上限（见 FlowerFarmFinderSearchRunner）---- */
constexpr int kLongSideMin = 256;
constexpr int kLongSideMax = 1024;
constexpr int kLongSideDefault = 640;
constexpr int kShortSideDefault = 320;
constexpr int kFrameStepFast = 16;    /* GUI Fast Mode */
constexpr int kFrameStepPrecise = 4;  /* 全窗口验证 */
constexpr int kDefaultRange = 16384;  /* 默认搜索范围 [-16384, 16383] */
constexpr long long kMaxSearchArea = 60000001LL * 60000001LL;
constexpr int kMaxThreads = 256;
constexpr const char *kDefaultMc = "26.3"; /* cubiomes MC_NEWEST，对应 UI 的 1.21.4+ */

const char *const kProgram = "FlowerFinderBase-cli";

struct Args {
    long long seed = 0;
    bool seedSet = false;
    int minX = -kDefaultRange;
    int maxX = kDefaultRange - 1;
    int minZ = -kDefaultRange;
    int maxZ = kDefaultRange - 1;
    int longSide = kLongSideDefault;
    int shortSide = kShortSideDefault;
    bool squareOnly = false;
    int frameStep = kFrameStepFast;
    int threads = 0; /* 0 = 自动（硬件线程数） */
    std::string mc = kDefaultMc;
    bool bothWildflowers = false;
    bool basaltDeltas = false;
    int orientation = FLOWER_ORIENT_ANY;
    int flowerCenterMargin = 0;
    std::string out;
    bool quiet = false;
};

/* 核心搜索的控制块，字段与 FlowerSearchControl 一一对应。 */
struct Progress {
    std::atomic_int current{0};
    std::atomic_int total{0};
    std::atomic_int phase{0};
    std::atomic_int overallCurrent{0};
    std::atomic_int overallTotal{0};
    std::atomic_int stage{0};
    std::atomic_int auxCurrent{0};
    std::atomic_int auxTotal{0};
    std::atomic_bool tryPause{false};
    std::atomic_bool tryStop{false};
};

std::atomic_bool g_stopRequested{false};

extern "C" void onSigint(int)
{
    g_stopRequested.store(true);
}

void printUsage(std::ostream &os)
{
    os <<
        "Usage:\n"
        "  " << kProgram << " --seed <int64> [options]\n"
        "\n"
        "Search range (block coordinates, inclusive; default -16384..16383):\n"
        "  --min-x <int>          minimum X\n"
        "  --max-x <int>          maximum X\n"
        "  --min-z <int>          minimum Z\n"
        "  --max-z <int>          maximum Z\n"
        "\n"
        "Farm window (same meaning as the GUI):\n"
        "  --long-side <int>      long window side in blocks, default " << kLongSideDefault << "\n"
        "                         (" << kLongSideMin << ".." << kLongSideMax << ")\n"
        "  --short-side <int>     short window side in blocks, default " << kShortSideDefault << "\n"
        "  --square-only          use the long side for both axes (square windows)\n"
        "  --frame-step <int>     CTS frame step, default " << kFrameStepFast << ";\n"
        "                         4 = full-window verification, larger = boundary screen\n"
        "  --fast                 shortcut for --frame-step " << kFrameStepFast << "\n"
        "  --precise              shortcut for --frame-step " << kFrameStepPrecise << "\n"
        "\n"
        "Result filters (all off by default):\n"
        "  --both-wildflowers     require birch/meadow (wildflowers) and cherry (pink petals)\n"
        "  --basalt-deltas        require basalt deltas in the nether counterpart\n"
        "  --orientation <o>      any | horizontal | vertical (default any)\n"
        "  --flower-center-margin <int>\n"
        "                         keep both flower-gradient ends at least this many\n"
        "                         blocks from every window edge, 0 = off\n"
        "\n"
        "Runtime:\n"
        "  --threads <int>        worker threads, default = CPU threads (max " << kMaxThreads << ")\n"
        "  --mc <version>         cubiomes version: 26.3 (default, UI 1.21.4+),\n"
        "                         1.21.4, 1.21.3, 1.18~1.21.3, legacy, latest,\n"
        "                         or a numeric MCVersion id\n"
        "  --out <file>           also write the /tp report to <file>\n"
        "  --quiet                suppress progress and summary on stderr\n"
        "  --help, -h             show this help\n"
        "  --version              show the version\n"
        "\n"
        "Output:\n"
        "  One /tp line per verified window on stdout (identical to the GUI\n"
        "  export), so --out produces a txt file ready to paste into a datapack\n"
        "  or a script. Progress and the summary go to stderr.\n"
        "\n"
        "Examples:\n"
        "  " << kProgram << " --seed 12345 --min-x -16384 --max-x 16383 \\\n"
        "      --min-z -16384 --max-z 16383 --threads 16 --out flower.txt\n"
        "  " << kProgram << " --seed -987654321 --precise --both-wildflowers --out hits.txt\n";
}

[[noreturn]] void usageError(const std::string &message)
{
    std::cerr << kProgram << ": " << message << "\n\n";
    printUsage(std::cerr);
    std::exit(1);
}

std::string trim(const std::string &s)
{
    const char *ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool parseLongLong(const std::string &text, long long &out)
{
    if (text.empty())
        return false;
    try {
        size_t used = 0;
        const long long value = std::stoll(text, &used, 10);
        if (used != text.size())
            return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseInt(const std::string &text, int &out)
{
    long long value = 0;
    if (!parseLongLong(text, value))
        return false;
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    out = static_cast<int>(value);
    return true;
}

int parseMcVersion(const std::string &text)
{
    const std::string value = trim(text);
    /* UI 里的版本区间标签，方便直接照抄 GUI 的选择 */
    if (value == "1.18~1.21.3" || value == "legacy")
        return str2mc("1.21.3");
    if (value == "1.21.4+" || value == "latest")
        return MC_NEWEST;

    const int mc = str2mc(value.c_str());
    if (mc != MC_UNDEF)
        return mc;

    /* 也接受 cubiomes MCVersion 的数值 id */
    int id = 0;
    if (parseInt(value, id) && id > MC_UNDEF && id <= MC_NEWEST)
        return id;
    return MC_UNDEF;
}

const char *phaseName(int phase)
{
    switch (phase) {
    case 0: return "preparing";
    case 1: return "climate screen";
    case 2: return "merge/cluster";
    case 3: return "verify";
    default: return "running";
    }
}

int parseOrientation(const std::string &text)
{
    const std::string value = trim(text);
    if (value == "any" || value == "0")
        return FLOWER_ORIENT_ANY;
    if (value == "horizontal" || value == "x" || value == "1")
        return FLOWER_ORIENT_HORIZONTAL;
    if (value == "vertical" || value == "z" || value == "2")
        return FLOWER_ORIENT_VERTICAL;
    return -1;
}

Args parseArgs(int argc, char **argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char *option) -> std::string {
            if (i + 1 >= argc)
                usageError("missing value for " + std::string(option));
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            std::exit(0);
        } else if (arg == "--version") {
            std::cout << kProgram << " " << FFB_VERSION << "\n";
            std::exit(0);
        } else if (arg == "--seed") {
            if (!parseLongLong(value("--seed"), args.seed))
                usageError("invalid --seed (expected a signed 64-bit integer)");
            args.seedSet = true;
        } else if (arg == "--min-x") {
            if (!parseInt(value("--min-x"), args.minX))
                usageError("invalid --min-x");
        } else if (arg == "--max-x") {
            if (!parseInt(value("--max-x"), args.maxX))
                usageError("invalid --max-x");
        } else if (arg == "--min-z") {
            if (!parseInt(value("--min-z"), args.minZ))
                usageError("invalid --min-z");
        } else if (arg == "--max-z") {
            if (!parseInt(value("--max-z"), args.maxZ))
                usageError("invalid --max-z");
        } else if (arg == "--long-side") {
            if (!parseInt(value("--long-side"), args.longSide))
                usageError("invalid --long-side");
        } else if (arg == "--short-side") {
            if (!parseInt(value("--short-side"), args.shortSide))
                usageError("invalid --short-side");
        } else if (arg == "--square-only") {
            args.squareOnly = true;
        } else if (arg == "--frame-step") {
            if (!parseInt(value("--frame-step"), args.frameStep))
                usageError("invalid --frame-step");
        } else if (arg == "--fast") {
            args.frameStep = kFrameStepFast;
        } else if (arg == "--precise") {
            args.frameStep = kFrameStepPrecise;
        } else if (arg == "--threads") {
            if (!parseInt(value("--threads"), args.threads))
                usageError("invalid --threads");
        } else if (arg == "--mc") {
            args.mc = trim(value("--mc"));
            if (args.mc.empty())
                usageError("--mc requires a non-empty version");
        } else if (arg == "--both-wildflowers") {
            args.bothWildflowers = true;
        } else if (arg == "--basalt-deltas") {
            args.basaltDeltas = true;
        } else if (arg == "--orientation") {
            args.orientation = parseOrientation(value("--orientation"));
            if (args.orientation < 0)
                usageError("invalid --orientation (expected any, horizontal or vertical)");
        } else if (arg == "--flower-center-margin") {
            if (!parseInt(value("--flower-center-margin"), args.flowerCenterMargin))
                usageError("invalid --flower-center-margin");
        } else if (arg == "--out") {
            args.out = value("--out");
            if (args.out.empty())
                usageError("--out requires a non-empty path");
        } else if (arg == "--quiet") {
            args.quiet = true;
        } else {
            usageError("unknown option: " + arg);
        }
    }
    return args;
}

void validate(const Args &args)
{
    if (!args.seedSet)
        usageError("--seed is required");
    if (args.minX > args.maxX)
        usageError("--min-x must not be greater than --max-x");
    if (args.minZ > args.maxZ)
        usageError("--min-z must not be greater than --max-z");

    const long long width = static_cast<long long>(args.maxX) - args.minX + 1;
    const long long height = static_cast<long long>(args.maxZ) - args.minZ + 1;
    if (width * height > kMaxSearchArea)
        usageError("search area is too large (max 60000001 x 60000001 blocks)");

    if (args.longSide < kLongSideMin || args.longSide > kLongSideMax)
        usageError("--long-side must be between " + std::to_string(kLongSideMin) +
                   " and " + std::to_string(kLongSideMax));
    if (args.shortSide < 1 || args.shortSide > kLongSideMax)
        usageError("--short-side must be between 1 and " + std::to_string(kLongSideMax));
    if (args.frameStep < 1)
        usageError("--frame-step must be positive");
    if (args.threads < 0 || args.threads > kMaxThreads)
        usageError("--threads must be between 0 and " + std::to_string(kMaxThreads));
    if (args.flowerCenterMargin < 0)
        usageError("--flower-center-margin must not be negative");
    if (parseMcVersion(args.mc) == MC_UNDEF)
        usageError("invalid --mc version: " + args.mc);
}

bool writeReport(const std::string &path, const std::vector<std::string> &lines,
                 std::string &error)
{
    std::ofstream out(path); /* 文本模式：与 GUI 导出一样按平台换行 */
    if (!out) {
        error = "cannot open '" + path + "' for writing";
        return false;
    }
    for (const std::string &line : lines)
        out << line << '\n';
    out.flush();
    if (!out) {
        error = "failed to write '" + path + "'";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);
    validate(args);

    const int mc = parseMcVersion(args.mc);
    unsigned hw = std::thread::hardware_concurrency();
    int maxThreads = hw > 0 ? static_cast<int>(hw) : 1;
    if (maxThreads > kMaxThreads)
        maxThreads = kMaxThreads;
    int threads = args.threads > 0 ? args.threads : maxThreads;
    if (threads < 1)
        threads = 1;
    if (threads > maxThreads)
        threads = maxThreads;

    const int shortSide = args.squareOnly ? args.longSide : args.shortSide;

    if (!args.quiet) {
        std::cerr << kProgram << " " << FFB_VERSION << "\n"
                  << "  seed      : " << args.seed << "\n"
                  << "  region    : x[" << args.minX << ", " << args.maxX
                  << "] z[" << args.minZ << ", " << args.maxZ << "]\n"
                  << "  window    : " << args.longSide << "x" << shortSide
                  << (args.squareOnly ? " (square)" : "") << "\n"
                  << "  frameStep : " << args.frameStep
                  << (args.frameStep <= kFrameStepPrecise ? " (full verify)" : "") << "\n"
                  << "  threads   : " << threads << "\n"
                  << "  mc        : " << args.mc << "\n";
        std::cerr.flush();
    }

    Progress progress;
    FlowerSearchControl control{
        &progress.current,
        &progress.total,
        &progress.phase,
        &progress.tryPause,
        &progress.tryStop,
        &progress.current,
        &progress.total,
        &progress.overallCurrent,
        &progress.overallTotal,
        &progress.stage,
        &progress.auxCurrent,
        &progress.auxTotal};

    FlowerSearchOptions options{};
    options.requireBothWildflowers = args.bothWildflowers;
    options.requireBasaltDeltas = args.basaltDeltas;
    options.orientation = args.orientation;
    options.flowerCenterMargin = args.flowerCenterMargin;

    std::signal(SIGINT, onSigint);

    std::atomic_bool monitorDone{false};
    std::thread monitor;
    if (!args.quiet) {
        monitor = std::thread([&progress, &monitorDone]() {
            while (!monitorDone.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                int total = progress.total.load();
                if (total <= 0)
                    continue;
                int phase = progress.phase.load();
                /* 屏幕后的 merge/cluster 尾段在 GUI 里是 phase 2 */
                if (phase == 1 && progress.stage.load() != 0)
                    phase = 2;
                int current = progress.current.load();
                if (phase == 2) {
                    const int auxTotal = progress.auxTotal.load();
                    if (auxTotal > 0) {
                        current = progress.auxCurrent.load();
                        total = auxTotal;
                    }
                }
                if (total <= 0)
                    continue;
                const double percent = current * 100.0 / total;
                std::fprintf(stderr, "\r[%-14s] %d/%d (%5.1f%%)   ",
                             phaseName(phase), current, total, percent);
                std::fflush(stderr);
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::string> lines;
    int exitCode = 0;
    try {
        lines = runFlowerFarmSearch(static_cast<uint64_t>(args.seed), mc,
                                    args.minX, args.minZ, args.maxX, args.maxZ,
                                    args.longSide, shortSide, args.squareOnly,
                                    args.frameStep, threads, control, options);
    } catch (const std::bad_alloc &) {
        monitorDone.store(true);
        if (monitor.joinable())
            monitor.join();
        std::cerr << "\n" << kProgram << ": out of memory\n";
        return 2;
    } catch (const std::exception &e) {
        monitorDone.store(true);
        if (monitor.joinable())
            monitor.join();
        std::cerr << "\n" << kProgram << ": search failed: " << e.what() << "\n";
        return 2;
    }

    monitorDone.store(true);
    if (monitor.joinable())
        monitor.join();

    const double elapsedMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

    if (!args.quiet) {
        std::fprintf(stderr, "\r%70s\r", ""); /* 清掉进度行 */
        std::fflush(stderr);
    }

    for (const std::string &line : lines)
        std::cout << line << '\n';
    std::cout.flush();

    if (!args.out.empty()) {
        std::string error;
        if (!writeReport(args.out, lines, error)) {
            std::cerr << kProgram << ": " << error << "\n";
            return 2;
        }
    }

    const bool stopped = g_stopRequested.load();
    if (!args.quiet) {
        std::fprintf(stderr, "%s: %zu window(s) in %.1f s%s\n",
                     kProgram, lines.size(), elapsedMs / 1000.0,
                     stopped ? " (stopped)" : "");
        if (!args.out.empty())
            std::fprintf(stderr, "%s: wrote %s\n", kProgram, args.out.c_str());
    }
    if (stopped)
        exitCode = 130;
    return exitCode;
}
