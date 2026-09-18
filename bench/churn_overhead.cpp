// Is the measured churn "pair" the allocator, or the harness?
//
// =============================================================================
// WHY THIS FILE EXISTS
// =============================================================================
// The churn loop in alloc_latency.cpp picks its victim slot with
//
//     uint32_t const i = rng() % g_live;
//
// where rng() is a 64-bit LCG and g_live is a uint32_t. That expression is
// EMITTED INTO THE TIMED INTERVAL. On the x86-64 host it is a few nanoseconds
// against a ~70 ns pair and can be ignored. On a 32-bit target it cannot: a
// 64x64->64 multiply and a 64-bit modulo by a runtime divisor are software
// routines there (`__umoddi3` is a loop), and they sit inside every measured
// interval -- the same failure mode bench_timer.h was written about, one level
// up. The instrument is no longer the clock; it is the address generator.
//
// The device run made this visible rather than theoretical: alloc_latency's pair
// figure came out at ~9.2 us on the ESP32-S3, which is ~2200 cycles for a
// free+alloc. Something in there was not the allocator. This benchmark says what.
//
// =============================================================================
// METHOD
// =============================================================================
// Four intervals, differing in exactly one thing each. Same batching discipline
// as bench_timer.h: one long interval per sample, minimum over trials.
//
//   D  loop + one array load            the floor of the measurement
//   C  address generation only          rng() % LIVE, no library calls
//   A  free + alloc, generation EXCLUDED (indices come from a pre-generated
//      ring, so the address sequence is still a real random sequence)
//   B  free + alloc, generation INCLUDED (this is alloc_latency.cpp's shape)
//
// Then, with D as the loop floor:
//   A - D = the allocator's own cost for one free+alloc
//   C - D = what the address generator costs
//   B - C = the same allocator cost, derived independently
// A - D and B - C are two estimates of one quantity, so their agreement is a
// cross-check on the whole construction rather than a restatement of it.
//
// The ring is filled from the very generator the harness uses, so row A is not
// measured on a hand-picked access pattern.
//
// Build (host):
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc bench/churn_overhead.cpp src/core.cpp -o build/bench_churn
//   ./build/bench_churn

#include "bench_timer.h"
#include "pondmerge/pondmerge.hpp"

#include <cstdint>
#include <cstdio>

// See alloc_latency.cpp: the on-target driver supplies one region shared by all
// benchmarks, and naming it needs external linkage.
#if defined(PM_BENCH_SHARED_ZONE)
extern uint8_t g_zone[];
#endif

namespace {

#ifndef PM_BENCH_ZONE_BYTES
#define PM_BENCH_ZONE_BYTES (256u * 1024u)
#endif
#ifndef PM_BENCH_SEGMENT
#define PM_BENCH_SEGMENT 4096u
#endif
#ifndef PM_BENCH_SEGMENTS
#define PM_BENCH_SEGMENTS 64u
#endif
#ifndef PM_BENCH_CHURN_LIVE
#define PM_BENCH_CHURN_LIVE 256u
#endif
#ifndef PM_BENCH_CHURN_OPS
#define PM_BENCH_CHURN_OPS 4096u
#endif
#ifndef PM_BENCH_IDX_RING
#define PM_BENCH_IDX_RING 1024u
#endif

constexpr uint32_t ZONE_BYTES = PM_BENCH_ZONE_BYTES;
constexpr uint32_t SEGMENT    = PM_BENCH_SEGMENT;
constexpr uint32_t SEGMENTS   = PM_BENCH_SEGMENTS;
constexpr uint32_t LIVE       = PM_BENCH_CHURN_LIVE;
constexpr uint32_t OPS        = PM_BENCH_CHURN_OPS;
constexpr uint32_t RING       = PM_BENCH_IDX_RING;
constexpr uint32_t TRIALS     = 8;
constexpr uint64_t SEED       = 0x9E3779B97F4A7C15ull;

static_assert(LIVE >= 2 && LIVE <= PM_MAX_OBJECTS,
              "the churn population must fit the descriptor table");
static_assert(RING >= 2 && (RING & (RING - 1)) == 0,
              "the index ring must be a power of two so it can be masked");
static_assert(SEGMENTS <= ZONE_BYTES / SEGMENT,
              "the pool cannot claim more segments than the zone contains");

#if !defined(PM_BENCH_SHARED_ZONE)
alignas(16) uint8_t g_zone[ZONE_BYTES];
#endif

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

// Minimum over trials of one long interval. bench_timer.h's rule, applied here:
// time a group of operations between two clock reads, never one operation.
template <class F>
uint64_t best_ns(F&& f) {
    uint64_t best = 0;
    for (uint32_t t = 0; t < TRIALS; ++t) {
        uint64_t const t0 = pm_bench::now_ns();
        f();
        uint64_t const dt = pm_bench::now_ns() - t0;
        if (best == 0 || dt < best) best = dt;
    }
    return best;
}

// One churn step at slot i: free, then re-allocate the same size. The size comes
// from the slot table rather than from the generator, so choosing the victim is
// the generator's whole job -- which is what row C measures.
inline void churn_at(uint32_t i) {
    uint32_t const sz = g_slots[i].size;
    (void)pm::free(g_slots[i].ref);
    pm::RawRef r{};
    (void)pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r);
    g_slots[i].ref = r;
}

// Live-slot bookkeeping, needed so the benchmark can hand the library back in a
// state deinit() accepts. deinit() refuses while any object is live (it returns
// Busy and leaves the instance initialised), so a benchmark that leaves objects
// behind blocks every later one: on the first run of the chained target driver
// this file failed to clean up and sections 3 and 4 both reported "init failed"
// instead of their measurements.
uint32_t g_live = 0;
uint32_t g_perm[LIVE];

bool fill_to(uint32_t n) {
    while (g_live < n) {
        uint32_t const sz = kSizes[g_live % kSizeCount];
        pm::RawRef r{};
        if (pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, g_live, r) != pm::Status::Ok)
            return false;
        g_slots[g_live].ref = r;
        g_slots[g_live].size = sz;
        ++g_live;
    }
    return true;
}

bool drain_to(uint32_t n) {
    while (g_live > n) {
        --g_live;
        if (pm::free(g_slots[g_live].ref) != pm::Status::Ok) return false;
    }
    return true;
}

void row(const char* what, uint64_t ns) {
    std::printf("%-52s %12llu %10.1f\n", what, (unsigned long long)ns,
                (double)ns / (double)OPS);
}

// Same layout, but for an interval that holds a different number of operations.
void row_n(const char* what, uint64_t ns, uint32_t ops) {
    std::printf("%-52s %12llu %10.1f\n", what, (unsigned long long)ns,
                (double)ns / (double)ops);
}

} // namespace

int pm_bench_churn_overhead() {
    pm_bench::report("churn harness decomposition: allocator vs address generator");
    std::printf("\nPondMerge churn harness decomposition\n");

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
    g_live = LIVE;

    // Filled BEFORE any interval, from the generator the harness itself uses, so
    // row A's address sequence is a real random sequence and not a hand-picked
    // one. Only its generation has been moved out of the measured window.
    g_rng = SEED;
    for (uint32_t n = 0; n < RING; ++n) g_idx[n] = rng() % LIVE;

    // A random ORDER for the free-only row, so that row does not free slots in
    // the order they happen to sit in the arena. Fisher-Yates over the same
    // generator, still outside every interval.
    for (uint32_t n = 0; n < LIVE; ++n) g_perm[n] = n;
    for (uint32_t n = LIVE; n > 1; --n) {
        uint32_t const j = rng() % n;
        uint32_t const t = g_perm[n - 1];
        g_perm[n - 1] = g_perm[j];
        g_perm[j] = t;
    }

    std::printf("live=%u  ops per interval=%u  trials=%u  index ring=%u\n",
                (unsigned)LIVE, (unsigned)OPS, (unsigned)TRIALS,
                (unsigned)RING);
    std::printf("zone=%u B  segments=%u  PM_MAX_OBJECTS=%d\n\n",
                (unsigned)ZONE_BYTES, (unsigned)SEGMENTS, (int)PM_MAX_OBJECTS);

    // D: the floor. A local accumulator and one array load per step, so the
    //    compiler cannot fold the loop into a closed form.
    uint64_t const d = best_ns([] {
        uint32_t acc = g_sink;
        for (uint32_t n = 0; n < OPS; ++n) acc += g_idx[n & (RING - 1)];
        g_sink = acc;
    });
    // C: address generation only. No library call at all.
    uint64_t const c = best_ns([] {
        g_rng = SEED;
        uint32_t acc = g_sink;
        for (uint32_t n = 0; n < OPS; ++n) acc += rng() % LIVE;
        g_sink = acc;
    });
    // A: the real thing, generation excluded.
    uint64_t const a = best_ns([] {
        for (uint32_t n = 0; n < OPS; ++n) churn_at(g_idx[n & (RING - 1)]);
    });
    // B: alloc_latency.cpp's shape, generation included.
    uint64_t const b = best_ns([] {
        g_rng = SEED;
        for (uint32_t n = 0; n < OPS; ++n) churn_at(rng() % LIVE);
    });

    // E/F: alloc alone and free alone, measured DIRECTLY. Legitimate here and not
    // on the measurement host: one interval on the device carries ~50 ns of
    // instrument bias against LIVE operations, i.e. well under 1 ns/op, whereas
    // on the host one interval carries ~7,500 ns and the differential
    // construction in alloc_latency.cpp exists for that reason. The bias per
    // operation is printed so a reader can tell which regime the run is in
    // without having to know the platform.
    //
    // Pattern caveat, stated because it limits what these two rows mean: an
    // allocation's address is chosen by first-fit, so a run of allocations
    // necessarily walks up the arena and there is no way to allocate "at a random
    // address". Row E therefore measures the FILL pattern, which is the easiest
    // case for an address-ordered structure. Row F frees in a pre-generated
    // random ORDER, because free does take an arbitrary object.
    uint32_t const rounds = 32;
    uint64_t best_alloc = 0, best_free = 0;
    uint32_t free_fail = 0;
    for (uint32_t t = 0; t < TRIALS; ++t) {
        for (uint32_t r = 0; r < rounds; ++r) {
            if (!drain_to(0)) break;
            uint64_t const t0 = pm_bench::now_ns();
            (void)fill_to(LIVE);
            uint64_t const dt_a = pm_bench::now_ns() - t0;
            uint64_t const t1 = pm_bench::now_ns();
            for (uint32_t n = 0; n < LIVE; ++n) {
                if (pm::free(g_slots[g_perm[n]].ref) != pm::Status::Ok) free_fail++;
            }
            g_live = 0;
            uint64_t const dt_f = pm_bench::now_ns() - t1;
            if (best_alloc == 0 || dt_a < best_alloc) best_alloc = dt_a;
            if (best_free == 0 || dt_f < best_free) best_free = dt_f;
        }
    }

    std::printf("%-52s %12s %10s\n", "component", "ns/interval", "ns/op");
    std::printf("%-52s %12s %10s\n",
                "----------------------------------------------------",
                "-----------", "-----");
    row("D  loop + one array load (measurement floor)", d);
    row("C  address generation only (rng + 64-bit modulo)", c);
    row("A  free+alloc, generation EXCLUDED (index ring)", a);
    row("B  free+alloc, generation INCLUDED (alloc_latency shape)", b);
    row_n("E  alloc only, fill pattern", best_alloc, LIVE);
    row_n("F  free only, random order", best_free, LIVE);

    {
        uint64_t const bias = pm_bench::interval_cost_ns();
        std::printf("\ninstrument bias: %llu ns per interval, i.e. %.3f ns per op"
                    " for rows E/F\n(and %.3f ns per op for rows A/B/C/D, which"
                    " hold %u ops each).\n",
                    (unsigned long long)bias, (double)bias / (double)LIVE,
                    (double)bias / (double)OPS, (unsigned)OPS);
        if (bias > (uint64_t)LIVE) {
            std::printf("WARNING: rows E/F are below the instrument's noise floor on"
                        " this platform --\n         subtract %.1f ns/op or do not"
                        " quote them.\n",
                        (double)bias / (double)LIVE);
        }
        if (free_fail) {
            std::printf("WARNING: %u of the row-F frees failed; the free figure is"
                        " not usable.\n", (unsigned)free_fail);
        }
    }

    double const n = (double)OPS;
    double const lib_a = ((double)a - (double)d) / n;
    double const gen_c = ((double)c - (double)d) / n;
    double const lib_b = ((double)b - (double)c) / n;

    std::printf("\nallocator cost, generation excluded (A - D) : %8.1f ns\n", lib_a);
    std::printf("allocator cost, differenced     (B - C)      : %8.1f ns\n", lib_b);
    std::printf("address generation cost         (C - D)      : %8.1f ns\n", gen_c);
    if ((double)b > 0) {
        std::printf("generation share of B                        : %8.1f %%\n",
                    100.0 * ((double)b - (double)a) / (double)b);
    }
    double const diff = lib_b - lib_a;
    double const gap = diff < 0 ? -diff : diff;
    double const ref = lib_a > 0 ? lib_a : 1.0;
    std::printf("\ncross-check: the two allocator estimates differ by %.1f ns"
                " (%.0f%% of A - D)\n", gap, 100.0 * gap / ref);
    std::printf("  They agree when the sum is additive, i.e. when the generator and"
                " the library\n  calls do not interact through cache or branch"
                " prediction. A large gap means the\n  two cannot be separated this"
                " way and the ring variant (A) is the one to quote.\n");

    std::printf("\nsink=%u (printed so the compiler cannot elide the timed loops)\n",
                (unsigned)g_sink);
    std::printf("validate: %s\n", pm::status_name(pm::validate(g_pool)));
    // Hand the instance back clean. The chained target driver deinit()s between
    // benchmarks and deinit() refuses while any object is live, so a benchmark
    // that leaves objects behind blocks every benchmark after it.
    std::printf("live objects at exit: %u (deinit requires 0)\n",
                (unsigned)g_live);
    return 0;
}

#ifndef PM_BENCH_NO_HOST_MAIN
int main() { return pm_bench_churn_overhead(); }
#endif
