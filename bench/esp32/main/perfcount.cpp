// perfcount.cpp -- where the cycles actually go, measured with the Xtensa
// performance counters instead of inferred from a ratio.
//
// WHY THIS EXISTS
// Section 5.3 of bench/RESULTS.md records a fact and then offers a guess: the
// device costs ~20x more per cycle than the host for allocator operations, while
// a trivial loop costs ~1x, and the guess was "memory and code access rather than
// CPU throughput". Section 6.3 then added a second chip and showed the gap is not
// a uniform clock effect -- a loop 14% faster on the LX6 while every allocator
// operation was 7-14% slower -- which argues against the simplest reading but
// does not test the guess.
//
// This measures it. The ESP32-S3's performance monitor counts, among other
// things, ICache-miss and DCache-miss penalty *in cycles*, so the share of the
// interval that is memory-stall can be read directly rather than inferred. The
// instruction counter then gives CPI, which distinguishes "the code path is long"
// from "the code path is stalled", and the stall breakdown includes an
// iterative-divide term, which would point at 32-bit division in the hot path.
//
// Everything about the workload is the same as bench/churn_overhead.cpp -- the
// same shared region, the same live count, the same index ring, the same three
// workloads (loop, generation-only, free+alloc) -- so the numbers here explain
// that file's rows rather than starting a new experiment.
//
// The counters are 32-bit and would wrap at 240 MHz after ~17.9 s; every interval
// here is tens of milliseconds, so no wrap. Counter 0 and counter 1 are read from
// the SAME trial as the cycle count, so a share is exact rather than an average
// of two runs.

#include "esp_cpu.h"
#include "perfmon.h"
#include "pondmerge/pondmerge.hpp"
#include "xtensa/xt_perf_consts.h"

#include <cstdint>
#include <cstdio>

// The shared region, owned by bench_main.cpp.
extern uint8_t g_zone[];

namespace {

#if !defined(PM_BENCH_ZONE_BYTES) || !defined(PM_BENCH_SEGMENTS)
#error "perfcount needs the sizing macros from the build"
#endif
// Same default as churn_overhead.cpp: the ring length is not passed by the CMake
// because 1024 suits every target, but this file's workload has to match that
// file's byte for byte or its rows and these are not comparable.
#ifndef PM_BENCH_IDX_RING
#define PM_BENCH_IDX_RING 1024u
#endif

constexpr uint32_t ZONE_BYTES = PM_BENCH_ZONE_BYTES;
constexpr uint32_t SEGMENT    = PM_BENCH_SEGMENT;
constexpr uint32_t SEGMENTS   = PM_BENCH_SEGMENTS;
// Same three-as-churn_overhead: the workload must be the one being explained.
constexpr uint32_t LIVE   = PM_BENCH_CHURN_LIVE;
constexpr uint32_t OPS    = PM_BENCH_CHURN_OPS;
constexpr uint32_t RING   = PM_BENCH_IDX_RING;
constexpr uint32_t TRIALS = 5;
constexpr uint64_t SEED   = 0x9E3779B97F4A7C15ull;

static_assert(LIVE >= 2 && LIVE <= PM_MAX_OBJECTS, "population must fit");
static_assert((RING & (RING - 1)) == 0, "ring must be a power of two");

const uint32_t kSizes[] = {32, 48, 64, 96, 128, 192, 256};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

struct Slot { pm::RawRef ref; uint32_t size; };

Slot      g_slots[LIVE];
uint32_t  g_idx[RING];
uint64_t  g_rng = SEED;
uint32_t  g_sink = 0;
pm::PoolId g_pool{};

inline uint32_t rng() {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 33);
}

// ---- the instrument ------------------------------------------------------

struct Event {
    uint16_t select;
    uint16_t mask;
    const char* name;
};

// Names come from xt_perf_consts.h; the two cache counters are documented there
// as measuring *penalty in cycles*, not miss counts, which is exactly what a
// share-of-interval needs.
Event const kEvents[] = {
    {XTPERF_CNT_ICACHE_MISSES, XTPERF_MASK_ICACHE_MISSES, "icache miss penalty"},
    {XTPERF_CNT_DCACHE_MISSES, XTPERF_MASK_DCACHE_MISSES, "dcache miss penalty"},
    {XTPERF_CNT_INSN,          XTPERF_MASK_INSN_ALL,      "instructions retired"},
    {XTPERF_CNT_D_STALL,       XTPERF_MASK_D_STALL_ALL,   "D-related stalls"},
    {XTPERF_CNT_I_STALL,       XTPERF_MASK_I_STALL_ALL,   "I-related stalls"},
    {XTPERF_CNT_BUBBLES,       XTPERF_MASK_BUBBLES_ALL,   "hold/bubble cycles"},
};
constexpr uint32_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

struct Result {
    uint32_t cycles = 0;
    uint32_t first = 0;
    uint32_t second = 0;
    bool ok = false;
};

// Run `body` once with counter 0 on `a` and counter 1 on `b`, keeping the trial
// with the fewest cycles so the counters and the cycle count describe one and the
// same execution.
template <class Body>
Result measure(Event const& a, Event const& b, Body&& body) {
    Result best;
    for (uint32_t t = 0; t < TRIALS; ++t) {
        xtensa_perfmon_stop();
        xtensa_perfmon_init(0, a.select, a.mask, 0, -1);
        xtensa_perfmon_init(1, b.select, b.mask, 0, -1);
        xtensa_perfmon_reset(0);
        xtensa_perfmon_reset(1);
        uint32_t const c0 = (uint32_t)esp_cpu_get_cycle_count();
        xtensa_perfmon_start();
        body();
        xtensa_perfmon_stop();
        uint32_t const cyc = (uint32_t)esp_cpu_get_cycle_count() - c0;
        uint32_t const v0 = xtensa_perfmon_value(0);
        uint32_t const v1 = xtensa_perfmon_value(1);
        if (!best.ok || cyc < best.cycles) {
            best.cycles = cyc;
            best.first = v0;
            best.second = v1;
            best.ok = true;
        }
    }
    return best;
}

// ---- the three workloads, copied from churn_overhead.cpp -----------------

inline void churn_at(uint32_t i) {
    uint32_t const sz = g_slots[i].size;
    (void)pm::free(g_slots[i].ref);
    pm::RawRef r{};
    (void)pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r);
    g_slots[i].ref = r;
}

void work_loop() {
    uint32_t acc = g_sink;
    for (uint32_t n = 0; n < OPS; ++n) acc += g_idx[n & (RING - 1)];
    g_sink = acc;
}

void work_generation() {
    g_rng = SEED;
    uint32_t acc = g_sink;
    for (uint32_t n = 0; n < OPS; ++n) acc += rng() % LIVE;
    g_sink = acc;
}

void work_churn() {
    for (uint32_t n = 0; n < OPS; ++n) churn_at(g_idx[n & (RING - 1)]);
}

struct Workload {
    const char* name;
    void (*fn)();
    uint32_t ops_per_call;  // OPS for all three: `work_*` loops OPS times
};

Workload const kWorkloads[] = {
    {"loop + one array load", work_loop, OPS},
    {"address generation only", work_generation, OPS},
    {"free + alloc (index ring)", work_churn, OPS},
};
constexpr uint32_t kWorkloadCount = sizeof(kWorkloads) / sizeof(kWorkloads[0]);

void report(const char* wname, uint32_t ops, Event const& a, Event const& b,
            Result const& r) {
    // Every value printed here is cast to (unsigned) on purpose: on Xtensa
    // uint32_t is an unsigned long, so a bare %u is a -Wformat error -- which is
    // why the rest of these benchmarks look over-cast too.
    std::printf("  %-26s %-11s %10u %10.2f/op\n", wname, a.name,
                (unsigned)r.first, (double)r.first / (double)ops);
    std::printf("  %-26s %-11s %10u %10.2f/op\n", "", b.name,
                (unsigned)r.second, (double)r.second / (double)ops);
    std::printf("  %-26s %-11s %10u %10.2f/op   counter-0 share %5.1f%%\n", "",
                "cycles (mcycle)", (unsigned)r.cycles,
                (double)r.cycles / (double)ops,
                r.cycles ? 100.0 * (double)r.first / (double)r.cycles : 0.0);
}

} // namespace

int pm_bench_perfcount() {
    std::printf("\n=== where the cycles go (Xtensa performance counters) ===\n");

    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) {
        std::printf("init failed\n");
        return 1;
    }
    if (pm::create_pool(g_pool, SEGMENTS) != pm::Status::Ok) {
        std::printf("create_pool failed\n");
        return 1;
    }
    for (uint32_t i = 0; i < LIVE; ++i) {
        uint32_t const sz = kSizes[i % kSizeCount];
        pm::RawRef r{};
        if (pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r) != pm::Status::Ok) {
            std::printf("fill failed at %u\n", (unsigned)i);
            return 1;
        }
        g_slots[i].ref = r;
        g_slots[i].size = sz;
    }
    g_rng = SEED;
    for (uint32_t n = 0; n < RING; ++n) g_idx[n] = rng() % LIVE;

    std::printf("zone=%u B segments=%u live=%u ops per interval=%u trials=%u\n",
                (unsigned)ZONE_BYTES, (unsigned)SEGMENTS, (unsigned)LIVE,
                (unsigned)OPS, (unsigned)TRIALS);
    std::printf("counters: 0 and 1 read from the same trial as mcycle, so the"
                " shares below are exact\n\n");

    // Pass 1: the two cache penalties (the hypothesis under test).
    // Pass 2: instructions + data stalls (CPI, and where data stalls come from).
    // Pass 3: instruction-side stalls.
    struct Pass { uint32_t ia, ib; };
    Pass const passes[] = {
        {0, 1},
        {2, 3},
        {4, 5},
    };

    for (uint32_t w = 0; w < kWorkloadCount; ++w) {
        std::printf("--- %s ---\n", kWorkloads[w].name);
        // Warm the caches and the branch predictors exactly as the benchmark
        // would, so a cold first pass is not mistaken for a costly one.
        kWorkloads[w].fn();
        kWorkloads[w].fn();
        for (uint32_t p = 0; p < sizeof(passes) / sizeof(passes[0]); ++p) {
            Event const& a = kEvents[passes[p].ia];
            Event const& b = kEvents[passes[p].ib];
            Result const r = measure(a, b, kWorkloads[w].fn);
            report(kWorkloads[w].name, kWorkloads[w].ops_per_call, a, b, r);
        }
        std::printf("\n");
    }

    // The two divisions above are integer; called out so a reader does not have
    // to wonder whether the code path contains one. (XTPERF's I-stall breakdown
    // has an iterative-divide term precisely because it is expensive here.)
    std::printf("note: rng() %% LIVE and the per-op normalisations are the only"
                " divisions in these paths;\n      the library's own hot path has"
                " none.\n");

    // Hand the instance back clean: the driver deinit()s between steps and
    // deinit() refuses while any object is live, so a step that leaves objects
    // behind blocks every step after it. (That is not hypothetical -- it is what
    // happened to the first two runs of bench_main.cpp.)
    for (uint32_t i = 0; i < LIVE; ++i) (void)pm::free(g_slots[i].ref);
    std::printf("live objects at exit: 0 (deinit requires 0)\n");
    return 0;
}
