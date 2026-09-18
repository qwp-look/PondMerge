// PondMerge allocator hot-path benchmark.
//
// Question: is alloc's documented O(bin chain + live objects) bound a real cost
// in practice, and which term dominates? NOT a fragmentation benchmark -- that
// is a separate, larger experiment (bench/fragmentation.cpp).
//
// READ bench/bench_timer.h FIRST. It explains why nothing here is timed as
// "t0 = now(); op(); t1 = now();". In short: on the measurement host one clock
// read costs ~9,300 ns and one measured interval carries a ~9,400 ns bias
// (the VM exit/entry round trip of a trapped RDTSC), against operations of
// ~40 ns. A per-operation timing there is not imprecise, it is meaningless.
//
// THREE MEASUREMENTS, ALL MADE THE SAME WAY
//
//   pair(L)  churn at a constant live count L: free an object and reallocate it
//            at a random position. The interval holds CHURN_PAIRS pairs, so it
//            lasts ~1.3 ms and the instrument bias is 0.7% of it. Measured
//            directly. This is the headline: it is what shows whether cost
//            tracks the live count.
//
//   T_fa(L)  one interval containing "free all L, then allocate all L".
//   T_a(L)   one interval containing "allocate all L" from an empty pool.
//            Both intervals contain exactly one clock bias, so
//                free(L) = (T_fa - T_a) / L
//            CANCELS the bias instead of trying to estimate it. The allocs in
//            T_fa run from live = 0 (the pool was just emptied), which is the
//            same trajectory as T_a, so the subtraction leaves pure free cost.
//
//   alloc(L) = pair(L) - free(L)      derived, and labelled as derived
//            (a churn pair is exactly one free plus one alloc at the same live
//             count, so the difference attributes the pair)
//
// Every interval is repeated and the MINIMUM is kept, with the median also
// reported. Interference on a shared host is purely additive (a stall never
// makes an interval faster), so the minimum is the right estimator, and the
// min-vs-median gap is printed as a direct read-out of how noisy the run was.
//
// The position freed and reallocated is chosen by a fixed-seed PRNG rather than
// taken from the tail. That matters for the before/after comparison: an
// insertion walk's cost depends on where the new element lands, so a fixed
// position would understate it. The PRNG is deterministic, so two builds see the
// same sequence of positions.
//
// Sizing is overridable so the same source can run on a host and on a target
// whose SRAM cannot afford the host defaults. The SHAPE of the curve is
// architecture-independent; the absolute values are not.
//
// Build (host), current core:
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc
//       bench/alloc_latency.cpp src/core.cpp -o build/bench_alloc
//   ./build/bench_alloc
//
// Build (host) against a DIFFERENT core revision, for the before/after A/B:
//   git show <rev>:src/core.cpp > /tmp/core_before.cpp
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc
//       bench/alloc_latency.cpp /tmp/core_before.cpp -o /tmp/bench_before
//   /tmp/bench_before
// The same benchmark source builds against both revisions because it only uses
// the public API, so the two runs differ in exactly one thing: the core.

#include "bench_timer.h"
#include "pondmerge/pondmerge.hpp"

#include <cstdint>
#include <cstdio>

namespace {

#ifndef PM_BENCH_ZONE_BYTES
#define PM_BENCH_ZONE_BYTES (256u * 1024u)
#endif
#ifndef PM_BENCH_SEGMENT
#define PM_BENCH_SEGMENT 4096u
#endif
#ifndef PM_BENCH_TARGET_LIVE
#define PM_BENCH_TARGET_LIVE 1024u
#endif
#ifndef PM_BENCH_CHURN_PAIRS
#define PM_BENCH_CHURN_PAIRS 16384u
#endif

constexpr uint32_t ZONE_BYTES  = PM_BENCH_ZONE_BYTES;
constexpr uint32_t SEGMENT     = PM_BENCH_SEGMENT;
constexpr uint32_t TARGET_LIVE = PM_BENCH_TARGET_LIVE;
constexpr uint32_t CHURN_PAIRS = PM_BENCH_CHURN_PAIRS;

static_assert(TARGET_LIVE <= PM_MAX_OBJECTS,
              "target live count must fit the descriptor table");
static_assert(ZONE_BYTES / SEGMENT <= PM_MAX_SEGMENTS,
              "the zone needs more segments than PM_MAX_SEGMENTS allows");
static_assert(TARGET_LIVE >= 16, "need room for at least one sample point");

alignas(16) uint8_t g_zone[ZONE_BYTES];

const uint32_t kSizes[] = {32, 48, 64, 96, 128, 192, 256};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

struct Slot { pm::RawRef ref; uint32_t size; };

Slot g_slots[TARGET_LIVE];
uint32_t g_live = 0;
pm::PoolId g_pool{};

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
inline uint32_t rng() {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 33);
}

// ---- untimed state setup ----------------------------------------------------

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

bool churn_one(uint32_t nops) {
    for (uint32_t n = 0; n < nops; ++n) {
        uint32_t const i = rng() % g_live;
        uint32_t const sz = g_slots[i].size;
        if (pm::free(g_slots[i].ref) != pm::Status::Ok) return false;
        pm::RawRef r{};
        if (pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r) != pm::Status::Ok)
            return false;
        g_slots[i].ref = r;
    }
    return true;
}

// ---- the three measurements -------------------------------------------------

// Churn pairs at constant live count L. One interval, measured directly and
// then repeated; the interval is long enough that the instrument is <1% of it.
void measure_pair(uint32_t L, double& min_ns, double& med_ns) {
    uint32_t const trials = 8;
    uint64_t v[8];
    for (uint32_t t = 0; t < trials; ++t) {
        if (!fill_to(L)) { min_ns = med_ns = -1; return; }
        uint64_t const t0 = pm_bench::now_ns();
        bool const ok = churn_one(CHURN_PAIRS);
        uint64_t const dt = pm_bench::now_ns() - t0;
        if (!drain_to(0)) { min_ns = med_ns = -1; return; }
        if (!ok) { min_ns = med_ns = -1; return; } // never let 0 become a minimum
        v[t] = dt;
    }
    uint64_t mn = v[0];
    for (uint32_t t = 1; t < trials; ++t)
        if (v[t] < mn) mn = v[t];
    // insertion sort, inline: the median is here for the reader to compare
    // against the minimum and see how noisy the run was.
    for (uint32_t i = 1; i < trials; ++i) {
        uint64_t const key = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; --j; }
        v[j] = key;
    }
    min_ns = (double)mn / CHURN_PAIRS;
    med_ns = (double)v[trials / 2] / CHURN_PAIRS;
}

// T_fa = free all L then allocate all L; T_a = allocate all L from empty.
// free(L) = (T_fa - T_a)/L, which cancels the instrument bias exactly.
//
// A differential of TWO LONG INTERVALS.
//
// The naive version -- "time L frees, time L allocs, subtract" -- does not work
// here: an L-operation interval is 0.6 us at L=16 and 41 us at L=1024, while one
// measured interval carries a ~9,400 ns bias whose spread reaches hundreds of
// microseconds. The differential drowns. (The first attempt at this file did
// exactly that and produced free = 43.8 ns at L=16 and unsigned-underflow noise
// in the error column -- a concrete demonstration, not a hypothetical.)
//
// Instead, build three intervals that each contain a LONG sequence of the same
// two batch operations and differ by exactly one batch:
//
//   I1 = (A F)^R          R alloc-batches, R       free-batches
//   I2 = (A F)^(R-1) A    R alloc-batches, R-1     free-batches
//   I3 = F (A F)^(R-1)    R-1 alloc-batches, R  free-batches
//
// where A = "allocate L" and F = "free L". Then
//
//   I1 - I2 = one free-batch      -> free(L)  = (I1 - I2) / L
//   I1 - I3 = one alloc-batch     -> alloc(L) = (I1 - I3) / L
//
// Every interval is milliseconds long, each carries exactly one bias, and the
// bias cancels in each subtraction. R is scaled with 1/L so that the interval
// length is roughly constant across sample points, which keeps the bias fraction
// constant and the comparison between rows meaningful.
//
// The pair figure is measured independently by churn, so pair == free + alloc is
// a genuine cross-check on this construction rather than an identity.
void measure_split(uint32_t L, double& free_ns, double& free_noise_ns,
                   double& alloc_ns, double& alloc_noise_ns) {
    free_ns = free_noise_ns = alloc_ns = alloc_noise_ns = -1;

    uint32_t R = 8192u / L;
    if (R < 8) R = 8;
    if (R > 512) R = 512;

    uint64_t i1_min = 0, i2_min = 0, i3_min = 0;
    uint64_t i1_max = 0, i2_max = 0, i3_max = 0;
    bool have = false;

    uint32_t const trials = 8;
    for (uint32_t t = 0; t < trials; ++t) {
        uint64_t i1 = 0, i2 = 0, i3 = 0;
        bool ok = drain_to(0); // I1 must start empty

        // I1 = (A F)^R   -- ends empty
        if (ok) {
            uint64_t const t0 = pm_bench::now_ns();
            for (uint32_t r = 0; r < R && ok; ++r) ok = fill_to(L) && drain_to(0);
            i1 = pm_bench::now_ns() - t0;
        }
        // I2 = (A F)^(R-1) A   -- ends holding L
        if (ok) {
            uint64_t const t0 = pm_bench::now_ns();
            for (uint32_t r = 0; r + 1 < R && ok; ++r) ok = fill_to(L) && drain_to(0);
            if (ok) ok = fill_to(L);
            i2 = pm_bench::now_ns() - t0;
        }
        // I3 = F (A F)^(R-1)   -- starts from L, ends empty
        if (ok) {
            ok = drain_to(0);
            uint64_t const t0 = pm_bench::now_ns();
            for (uint32_t r = 0; r + 1 < R && ok; ++r) ok = fill_to(L) && drain_to(0);
            i3 = pm_bench::now_ns() - t0;
        }
        if (!ok) return; // leave all four outputs at -1

        if (!have) {
            i1_min = i1_max = i1;
            i2_min = i2_max = i2;
            i3_min = i3_max = i3;
            have = true;
        } else {
            if (i1 < i1_min) i1_min = i1;
            if (i1 > i1_max) i1_max = i1;
            if (i2 < i2_min) i2_min = i2;
            if (i2 > i2_max) i2_max = i2;
            if (i3 < i3_min) i3_min = i3;
            if (i3 > i3_max) i3_max = i3;
        }
    }
    if (!have) return;

    // Signed arithmetic: the spread can legitimately make a max-difference
    // small or negative, and an unsigned subtraction would wrap.
    double const fmin = (double)((int64_t)i1_min - (int64_t)i2_min) / (double)L;
    double const amin = (double)((int64_t)i1_min - (int64_t)i3_min) / (double)L;
    double const fmax = (double)((int64_t)i1_max - (int64_t)i2_max) / (double)L;
    double const amax = (double)((int64_t)i1_max - (int64_t)i3_max) / (double)L;
    free_ns = fmin;
    alloc_ns = amin;
    free_noise_ns = (fmax > fmin) ? (fmax - fmin) : 0.0;
    alloc_noise_ns = (amax > amin) ? (amax - amin) : 0.0;
}

struct Row {
    uint32_t live;
    double pair_ns, pair_med_ns;      // churn, measured directly
    double free_ns, free_noise_ns;    // differential I1-I2
    double alloc_ns, alloc_noise_ns;  // differential I1-I3
    double split_pair_ns;             // free + alloc, from the differentials
    double gap_ns;                    // churn pair - split pair
    bool split_ok;                    // false = not attempted / below the floor
    bool split_trusted;               // false = attempted, cross-check disagreed
};

// A pre-filter only: skip the attribution where it is obviously hopeless. The
// REAL validity test is the cross-check further down, which compares the
// attribution against the independently measured churn pair and marks the row
// when the two disagree. A guessed threshold cannot know the interval jitter,
// which is what actually limits this measurement; the cross-check can.
constexpr double kSplitSnrFloor = 20.0;
constexpr double kSplitGapLimit = 0.20; // cross-check tolerance, fraction of pair

Row sample(uint32_t L) {
    Row r{};
    r.live = L;
    r.pair_ns = r.pair_med_ns = -1;
    r.free_ns = r.free_noise_ns = -1;
    r.alloc_ns = r.alloc_noise_ns = -1;
    r.split_pair_ns = r.gap_ns = -1;
    r.split_ok = false;
    r.split_trusted = false;

    measure_pair(L, r.pair_ns, r.pair_med_ns);

    // Decide whether the split is even worth attempting at this L. The pair
    // tells us what one operation costs, so the signal size is known before
    // measuring it.
    double const op_est = (r.pair_ns > 0) ? r.pair_ns * 0.5 : 40.0;
    double const bias = (double)pm_bench::call_cost_ns();
    if (r.pair_ns > 0 && (double)L * op_est >= kSplitSnrFloor * bias) {
        measure_split(L, r.free_ns, r.free_noise_ns, r.alloc_ns, r.alloc_noise_ns);
        if (r.free_ns >= 0 && r.alloc_ns >= 0) {
            r.split_pair_ns = r.free_ns + r.alloc_ns;
            r.gap_ns = r.pair_ns - r.split_pair_ns;
            r.split_ok = true;
            double const g = r.gap_ns < 0 ? -r.gap_ns : r.gap_ns;
            r.split_trusted = (g <= kSplitGapLimit * r.pair_ns);
        }
    }
    return r;
}

} // namespace

int pm_bench_alloc_latency() {
    pm_bench::report("alloc latency: pair / free / alloc against live count");

    pm::Config cfg{g_zone, sizeof(g_zone), SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) {
        std::printf("init failed\n");
        return 1;
    }
    if (pm::create_pool(g_pool, 64) != pm::Status::Ok) {
        std::printf("create_pool failed\n");
        return 1;
    }
    std::printf("zone %u B  target live %u  metadata_bytes %u\n",
                (unsigned)ZONE_BYTES, (unsigned)TARGET_LIVE,
                (unsigned)pm::global_stats().metadata_bytes);
    std::printf("pair: churn interval of %u pairs, measured directly.\n"
                "free/alloc: differentials of three long intervals"
                " (see measure_split).\n"
                "All figures are the minimum over trials; the median is printed"
                " next to pair\nso the run's noise level is visible.\n",
                (unsigned)CHURN_PAIRS);

    std::printf("\n%6s %9s %9s %9s %9s %9s %9s %11s %8s  %s\n", "live",
                "pair ns", "pair med", "free ns", "free +-", "alloc ns",
                "alloc +-", "free+alloc", "gap", "attribution");
    std::printf("%6s %9s %9s %9s %9s %9s %9s %11s %8s  %s\n", "----", "-------",
                "-------", "-------", "-------", "-------", "-------",
                "----------", "------", "-----------");

    Row rows[16];
    uint32_t nrow = 0;
    uint32_t nsplit = 0;
    for (uint32_t L = 16; L <= TARGET_LIVE && nrow < 16; L *= 2) {
        rows[nrow] = sample(L);
        Row const& r = rows[nrow];
        if (r.split_ok) {
            if (r.split_trusted) ++nsplit;
            std::printf("%6u %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %11.1f %8.1f  %s\n",
                        (unsigned)r.live, r.pair_ns, r.pair_med_ns, r.free_ns,
                        r.free_noise_ns, r.alloc_ns, r.alloc_noise_ns,
                        r.split_pair_ns, r.gap_ns,
                        r.split_trusted ? "ok" : "REJECTED (cross-check)");
        } else {
            std::printf("%6u %9.1f %9.1f %9s %9s %9s %9s %11s %8s  %s\n",
                        (unsigned)r.live, r.pair_ns, r.pair_med_ns, "n/a", "-",
                        "n/a", "-", "n/a", "-", "n/a (below floor)");
        }
        std::fflush(stdout);
        ++nrow;
        if (L > TARGET_LIVE / 2) break;
    }

    if (!nsplit) {
        std::printf("\nNo TRUSTED attribution (free vs alloc) at this size.\n"
                    "  The attribution is a differential of three long intervals"
                    " and is only as good as\n"
                    "  the interval-to-interval jitter, which a ~%llu ns clock"
                    " makes large. Rows are\n"
                    "  marked REJECTED when the attribution disagrees with the"
                    " independently measured\n"
                    "  churn pair by more than %.0f%%. Raise the configuration"
                    " (see bench/README.md);\n"
                    "  on the device, or on a host with a working cycle counter,"
                    " the restriction\n"
                    "  does not apply at all.\n",
                    (unsigned long long)pm_bench::call_cost_ns(),
                    kSplitGapLimit * 100.0);
    }

    if (nrow >= 2) {
        // Report the scaling over the first and last rows that have a figure.
        // For the pair metric every row has one; for the attribution only the
        // trusted rows do, so those endpoints are taken explicitly.
        Row const& a = rows[0];
        Row const& b = rows[nrow - 1];
        double const span = (double)b.live / (double)a.live;
        std::printf("\nscaling live %u -> %u (%.0fx). An O(live) term would give"
                    " ratios near %.0fx; O(1) gives ~1.0:\n",
                    (unsigned)a.live, (unsigned)b.live, span, span);
        std::printf("  pair      %8.1f -> %8.1f ns   x%.2f   (%u rows)\n",
                    a.pair_ns, b.pair_ns, b.pair_ns / a.pair_ns, (unsigned)nrow);

        Row const* fa = nullptr;
        Row const* fb = nullptr;
        for (uint32_t i = 0; i < nrow; ++i) {
            if (!rows[i].split_trusted) continue;
            if (!fa) fa = &rows[i];
            fb = &rows[i];
        }
        if (fa && fb && fa != fb) {
            std::printf("  free      %8.1f -> %8.1f ns   x%.2f   (%u trusted rows)\n",
                        fa->free_ns, fb->free_ns, fb->free_ns / fa->free_ns,
                        (unsigned)nsplit);
            std::printf("  alloc     %8.1f -> %8.1f ns   x%.2f\n", fa->alloc_ns,
                        fb->alloc_ns, fb->alloc_ns / fa->alloc_ns);
        }

        double worst_gap = 0, worst_frac = 0;
        for (uint32_t i = 0; i < nrow; ++i) {
            if (!rows[i].split_trusted) continue;
            double const g =
                rows[i].gap_ns < 0 ? -rows[i].gap_ns : rows[i].gap_ns;
            if (g > worst_gap) worst_gap = g;
            if (rows[i].pair_ns > 0) {
                double const fr = g / rows[i].pair_ns;
                if (fr > worst_frac) worst_frac = fr;
            }
        }
        if (nsplit) {
            std::printf("  cross-check: |churn pair - (free+alloc)| <= %.1f ns"
                        " (%.1f%%), over %u trusted of %u rows\n",
                        worst_gap, 100.0 * worst_frac, (unsigned)nsplit,
                        (unsigned)nrow);
        }
    }

    pm::PoolStats const s = pm::get_stats(g_pool);
    std::printf("\nlargest_free_block=%u free_bytes=%u fragment=%u\n",
                (unsigned)s.largest_free_block, (unsigned)s.free_bytes,
                (unsigned)s.fragment_bytes);
    std::printf("validate: %s\n", pm::status_name(pm::validate(g_pool)));
    pm::GlobalStats const g1 = pm::global_stats();
    std::printf("metadata_bytes=%u max_live=%u\n", (unsigned)g1.metadata_bytes,
                (unsigned)g1.max_live_objects);
    return 0;
}

// Host builds get a main(); the on-target build defines PM_BENCH_NO_HOST_MAIN
// and calls pm_bench_alloc_latency() from app_main instead.
#ifndef PM_BENCH_NO_HOST_MAIN
int main() { return pm_bench_alloc_latency(); }
#endif
