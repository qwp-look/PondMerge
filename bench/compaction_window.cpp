// PondMerge compaction-window benchmark -- what does ONE compact() call cost,
// as a DISTRIBUTION rather than a single point?
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
// fragmentation.cpp triggers compaction only on a failed probe, and after the
// first compact() heals the scattered holes the same-size churn cannot
// re-fragment the pool -- so a full run of that benchmark contains exactly ONE
// compaction event (on every platform; see RESULTS.md sections 4-6). One event
// gives one number. It cannot answer "how much does a compact cost when the
// fragmentation pattern is different", and it cannot support a percentile.
//
// The device clock makes per-event timing cheap (an mcycle read: ~59 ns on
// the S3 against a compaction of several ms), and bench/README.md has said
// all along that "percentiles belong on the device". This file collects them.
//
// ---------------------------------------------------------------------------
// THE REGIME, AND WHY EACH CYCLE LOOKS LIKE THIS
// ---------------------------------------------------------------------------
// The measured regime is the one fragmentation.cpp's single event ran in, so
// this distribution extends that record instead of measuring a different
// experiment. Setup reproduces its state exactly:
//
//   fill     steep size mix to exhaustion, pinned fence post every 16th
//            object (identical constants);
//   scatter  release every 4th slot -- which, exactly as in
//            fragmentation.cpp, includes the pinned posts themselves
//            (every 16th slot is also a 4th slot). This is measured, not
//            assumed: a probe run of this exact sequence reports
//            has_pinned=0 after the scatter, and the consolidation then
//            moves the same 189 objects / 184,296 B the host record shows.
//            The pinned posts shape the fill; they are not present during
//            the measured phase;
//   settle   ONE untimed compact() (the same event RESULTS.md records) plus
//            a refill to exhaustion, so every timed event starts from the
//            same regime: a FULL pool, every object movable, no pins.
//
// Then each timed cycle:
//
//   1. free K movable objects at random (K in [K_MIN, K_MIN+K_SPAN]).
//      The pool is full, so these are dispersed holes of <= 2 KiB + header
//      each, surrounded by live neighbours.
//   2. demand PROBE_BYTES contiguously. A single hole cannot satisfy it and
//      K random frees rarely coalesce into a run that can: the demand
//      FAILS, which is the product's compaction trigger.
//   3. read-only advice first (as a product would do), then time ONE
//      compact() call -- that duration is the compaction window, the time
//      the pool is paused for maintenance. Record it together with the
//      objects/bytes compact actually moved (PoolStats reports the last
//      compaction) and with the advice's move estimate.
//   4. retry the probe: with no pins, compact consolidates ALL free space
//      into one tail block of roughly the K freed blocks, so the retry
//      usually succeeds. Whatever the outcome, REFILL the exact sizes freed
//      in step 1, in the same order, which returns the pool to full -- so
//      every cycle starts from the same occupancy and only the hole pattern
//      differs.
//
// Step 4 is what makes repeated events possible at all: without the refill,
// the first compact() would leave one large tail block and every later probe
// would succeed from it, and the run would contain one event no matter how
// long it ran.
//
// K_MIN is chosen so the post-compact tail (the K freed blocks, ~1 KiB on
// average in this size mix) usually still satisfies the probe retry on both
// the host (probe 12 KiB) and the device (probe 10 KiB). "Usually", not
// "always": the freed sizes are drawn from the live mix, so a K drawn small
// with small objects can leave a tail under the probe. Events where the
// retry still fails are counted and reported, not hidden -- the window
// duration is valid either way, and "compact did not rescue this demand" is
// a real product outcome.
//
// ---------------------------------------------------------------------------
// WHY THE MEASURED PHASE HAS NO PINS (measured, then kept out on purpose)
// ---------------------------------------------------------------------------
// A development build of this benchmark kept the pinned posts alive during
// the cycles (freeing only movables, refilling to full). With pins every 16
// objects, compact() packs each inter-pin run separately: every barrier
// strands the free space that accumulated below it, and the consolidated
// state is a set of ~1 KiB pockets rather than one tail block. A 10-12 KiB
// probe retry then fails after EVERY compaction -- rescued=0 over 512 events.
// That is a real property of dense barriers (COMPACTION_POLICY.md section 7
// predicts it qualitatively), but it is NOT the regime the recorded
// fragmentation numbers come from, and a window distribution whose trigger
// flow never completes would measure consolidation without the product's
// "compact to satisfy a demand" story. The pins therefore stay where
// fragmentation.cpp has them: in the fill, gone before measurement.
//
// ---------------------------------------------------------------------------
// WHAT IS NOT CLAIMED HERE
// ---------------------------------------------------------------------------
// This is a single-variant distribution measurement, so fragmentation.cpp's
// A/B fairness rules (never compact from the churn path; two variants share
// one PRNG order) do not apply: there is no second variant, and compaction is
// still triggered only after a FAILED probe, never from the refill path. The
// percentiles are quotable ONLY on the device, where the clock is a register
// read; on the host each per-event interval carries the instrument bias
// (bench_timer.h reports it) and the numbers below are shape only.
//
// Build (host):
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc bench/compaction_window.cpp src/core.cpp -o build/bench_cw
//   ./build/bench_cw

#include "bench_timer.h"
#include "pondmerge/pondmerge.hpp"

#include <cstdint>
#include <cstdio>

// Same shared-region arrangement as the other benchmarks: the on-target
// driver owns the one scratch region and every benchmark takes its turn.
#if defined(PM_BENCH_SHARED_ZONE)
extern uint8_t g_zone[];
#endif

namespace {

#ifndef PM_BENCH_POPULATION
// 512 on the host, so the settle refill can always reach true exhaustion
// (the region, not this cap, ends the refill) and the refs array below
// bounds the TOTAL live set, not just the initial fill. The device CMake
// overrides this per target (192 KiB / 112 KiB regions hold fewer objects
// than the 192/128 the device build sets).
#define PM_BENCH_POPULATION 512u
#endif
#ifndef PM_BENCH_PROBE_BYTES
#define PM_BENCH_PROBE_BYTES (12u * 1024u)
#endif
#ifndef PM_BENCH_CW_EVENTS
#define PM_BENCH_CW_EVENTS 512u
#endif
#ifndef PM_BENCH_CW_K_MIN
#define PM_BENCH_CW_K_MIN 14u
#endif
#ifndef PM_BENCH_CW_K_SPAN
#define PM_BENCH_CW_K_SPAN 6u   // K = K_MIN + rng() % (K_SPAN + 1) -> [14, 20]
#endif
#ifndef PM_BENCH_PINNED_EVERY
#define PM_BENCH_PINNED_EVERY 16u
#endif
#ifndef PM_BENCH_SCATTER_MOD
#define PM_BENCH_SCATTER_MOD 4u
#endif
#ifndef PM_BENCH_REGION_BYTES
#define PM_BENCH_REGION_BYTES (256u * 1024u)
#endif

constexpr uint32_t REGION_BYTES = PM_BENCH_REGION_BYTES;
constexpr uint32_t SEGMENT      = 4096u;
constexpr uint32_t POPULATION   = PM_BENCH_POPULATION;
constexpr uint32_t EVENTS       = PM_BENCH_CW_EVENTS;
constexpr uint32_t K_MAX        = PM_BENCH_CW_K_MIN + PM_BENCH_CW_K_SPAN;
constexpr uint32_t PROBE_BYTES  = PM_BENCH_PROBE_BYTES;
constexpr uint32_t PINNED_EVERY = PM_BENCH_PINNED_EVERY;
constexpr uint32_t SCATTER_MOD  = PM_BENCH_SCATTER_MOD;
// A run of cycles that records no event (probe keeps succeeding, or compact
// keeps being refused) means the workload has stopped doing what it claims;
// stop and say so instead of spinning or hanging the console.
constexpr uint32_t kMaxFruitless = 64u;

static_assert(POPULATION <= PM_MAX_OBJECTS,
              "the population must fit in the descriptor table");
static_assert(PINNED_EVERY >= 2, "PINNED_EVERY=1 would pin everything");
static_assert(PM_BENCH_CW_K_MIN >= 2, "freeing < 2 objects cannot fragment");

// Steep size mix: identical to fragmentation.cpp, so the fill/scatter state
// here is the same one whose single compaction RESULTS.md already records.
const uint32_t kSizes[] = {256, 512, 768, 1024, 1536, 2048};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

#if !defined(PM_BENCH_SHARED_ZONE)
alignas(16) uint8_t g_zone[REGION_BYTES];
#endif

uint32_t g_rng;
inline uint32_t rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

inline uint64_t now_ns() { return pm_bench::now_ns(); }

// One recorded compaction window. The library's own compact_time_us (an
// esp_timer reading on the device, microseconds) is kept alongside the
// mcycle-derived figure as an independent cross-check of the instrument.
struct Sample {
    uint32_t dt_ns;          // the window: one compact() call, mcycle-derived
    uint32_t moved_bytes;    // PoolStats.bytes_moved (last compaction)
    uint32_t est_bytes;      // advice's estimated_moved_bytes, or UNKNOWN
    uint16_t moved_objects;  // PoolStats.objects_moved (last compaction)
    uint16_t lib_us;         // PoolStats.compact_time_us, clamped to u16 max
};
Sample g_samples[EVENTS];

void print_percentile_line(const char* unit, const uint32_t* v, uint32_t n,
                           double scale) {
    // v is sorted ascending; nearest-rank percentiles.
    printf("    %-3s p50=%10.3f  p75=%10.3f  p90=%10.3f  p95=%10.3f"
           "  p99=%10.3f\n",
           unit, (double)v[(n * 50u + 99u) / 100u - 1u] / scale,
           (double)v[(n * 75u + 99u) / 100u - 1u] / scale,
           (double)v[(n * 90u + 99u) / 100u - 1u] / scale,
           (double)v[(n * 95u + 99u) / 100u - 1u] / scale,
           (double)v[(n * 99u + 99u) / 100u - 1u] / scale);
}

int run_window() {
    static pm::RawRef refs[POPULATION];
    static uint32_t sizes[POPULATION];
    static uint32_t freed_sizes[K_MAX];
    static uint32_t sorted[EVENTS];
    static uint32_t order[EVENTS];
    uint32_t live = 0;

    pm::Config cfg{g_zone, REGION_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) { printf("init failed\n"); return 1; }
    pm::PoolId pool{};
    if (pm::create_pool(pool, REGION_BYTES / SEGMENT) != pm::Status::Ok) {
        printf("create_pool failed\n"); return 1;
    }

    // ---- fill to exhaustion, pinned fence posts every PINNED_EVERY-th -----
    g_rng = 0x2545F491u;  // fixed seed: the whole run is reproducible
    for (uint32_t i = 0; i < POPULATION; ++i) {
        uint32_t const size = kSizes[i % kSizeCount];
        bool const pin = ((i + 1) % PINNED_EVERY) == 0;
        pm::RawRef ref{};
        pm::Status st = pm::alloc(pool, size, 8,
                                  pin ? uint16_t(pm::PM_PINNED)
                                      : uint16_t(pm::PM_MOVABLE),
                                  i, ref);
        if (st != pm::Status::Ok) break;  // region exhausted
        refs[live++] = ref;
        sizes[live - 1u] = size;
    }
    uint32_t const filled = live;
    printf("  filled=%u objects (pinned every %u-th), region=%u B,"
           " segment=%u B\n", (unsigned)filled, (unsigned)PINNED_EVERY,
           (unsigned)REGION_BYTES, (unsigned)SEGMENT);

    // ---- scatter: release every SCATTER_MOD-th slot, pins included --------
    // Byte-for-byte the shape of fragmentation.cpp's phase 2, including its
    // effect on the pins -- see the header comment for why that is stated
    // here as a measured fact rather than glossed over.
    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < live; ++i) {
            if (((i + 1) % SCATTER_MOD) == 0) { (void)pm::free(refs[i]); continue; }
            refs[w] = refs[i]; sizes[w] = sizes[i]; w++;
        }
        live = w;
    }
    printf("  after scatter: live=%u (pinned posts included in the releases,"
           " as in fragmentation.cpp)\n", (unsigned)live);

    // ---- settle: one UNTIMED consolidation, then refill to full -----------
    // This is the same compaction event RESULTS.md records for the
    // fragmentation benchmark (same state, same seed family), taken out of
    // the timed path: the timed regime below is "full pool, K dispersed
    // holes", and without this settle the scatter's free space would let the
    // first probes succeed without any compaction at all.
    pm::CompactionRequest req{PROBE_BYTES, 8, 0, 0};
    {
        pm::Status const cs = pm::compact(pool);
        if (cs != pm::Status::Ok) {
            printf("  settle compact failed: %s -- aborting\n",
                   pm::status_name(cs));
            (void)pm::resume(pool);
            (void)pm::deinit();
            return 1;
        }
        pm::PoolStats s = pm::get_stats(pool);
        printf("  settle compact (untimed): moved %u objects / %u B;"
               " refilling to full\n", (unsigned)s.objects_moved,
               (unsigned)s.bytes_moved);
        for (uint32_t i = 0; i < POPULATION && live < POPULATION; ++i) {
            uint32_t const size = kSizes[i % kSizeCount];
            pm::RawRef r{};
            if (pm::alloc(pool, size, 8, pm::PM_MOVABLE, i, r)
                != pm::Status::Ok) break;  // full again
            refs[live] = r; sizes[live] = size; live++;
        }
    }
    printf("  regime: full pool (%u live, all movable), cycles of"
           " free-K / failed probe / ONE timed compact / refill\n",
           (unsigned)live);

    // Append replacements for exactly the sizes freed this cycle, in the
    // order they were freed. Post-compact the free space is one tail block
    // (no pins), so first-fit lays these out back-to-back at the top; the
    // pool is full again and only the hole pattern of the NEXT cycle differs.
    uint32_t refill_refusals = 0;
    auto refill_freed = [&](uint32_t k, uint32_t tag) {
        for (uint32_t j = 0; j < k; ++j) {
            pm::RawRef r{};
            if (pm::alloc(pool, freed_sizes[j], 8, pm::PM_MOVABLE, tag, r)
                != pm::Status::Ok) { refill_refusals++; continue; }
            refs[live] = r; sizes[live] = freed_sizes[j]; live++;
        }
    };

    uint32_t events = 0, cycles = 0, probe_ok_pre = 0, rescued = 0;
    uint32_t still_failed = 0, compact_refused = 0;
    uint32_t fruitless = 0, est_unknown = 0;
    uint32_t max_est_objects_dev = 0, max_est_bytes_dev = 0, max_lib_us_dev = 0;
    uint32_t moved_bytes_min = 0, moved_bytes_max = 0;
    uint64_t moved_bytes_sum = 0;

    while (events < EVENTS && cycles < EVENTS * 20u) {
        cycles++;
        if (live <= K_MAX + 1u) {
            printf("  live=%u too small to fragment at cycle %u -- stopping\n",
                   (unsigned)live, (unsigned)cycles);
            break;
        }

        // ---- 1. free K random movable objects (swap-remove from live set) -
        uint32_t const k_want =
            PM_BENCH_CW_K_MIN + rng() % (PM_BENCH_CW_K_SPAN + 1u);
        uint32_t k = 0, guard = 0;
        while (k < k_want && guard < k_want * 64u) {
            guard++;
            uint32_t const i = rng() % live;
            if (pm::free(refs[i]) != pm::Status::Ok) continue;
            freed_sizes[k] = sizes[i];
            live--;
            refs[i] = refs[live]; sizes[i] = sizes[live];
            k++;
        }
        if (k < PM_BENCH_CW_K_MIN) {
            // Should not happen at these occupancies; if it ever does, stop
            // rather than measure a state this file does not describe.
            printf("  only %u of %u frees succeeded at cycle %u -- stopping\n",
                   (unsigned)k, (unsigned)k_want, (unsigned)cycles);
            break;
        }

        // ---- 2. the probe demand (the product's compaction trigger) -------
        pm::RawRef probe{};
        if (pm::alloc(pool, PROBE_BYTES, 8, pm::PM_MOVABLE, 0xFFFFu, probe)
            == pm::Status::Ok) {
            // Rare: enough frees coalesced into a run that already fits the
            // demand. Free it, refill, and try a fresh pattern -- no event,
            // because no compaction happened.
            (void)pm::free(probe);
            probe_ok_pre++;
            fruitless++;
            refill_freed(k, cycles);
            if (fruitless > kMaxFruitless) {
                printf("  %u cycles without a single failed probe -- workload"
                       " stopped fragmenting; stopping\n",
                       (unsigned)kMaxFruitless);
                break;
            }
            continue;
        }

        // ---- 3. read-only advice, then ONE timed compact() ----------------
        pm::CompactionAdvice adv = pm::analyze_compaction(pool, &req);
        uint64_t const t0 = now_ns();
        pm::Status const cs = pm::compact(pool);
        uint64_t const dt = now_ns() - t0;
        if (cs != pm::Status::Ok) {
            compact_refused++;
            fruitless++;
            (void)pm::resume(pool);  // a refused compact stays Paused
            refill_freed(k, cycles);
            if (fruitless > kMaxFruitless) {
                printf("  %u cycles without a recorded event (last: compact"
                       " refused) -- stopping\n", (unsigned)kMaxFruitless);
                break;
            }
            continue;
        }
        fruitless = 0;
        pm::PoolStats s = pm::get_stats(pool);

        Sample& sp = g_samples[events];
        sp.dt_ns = (uint32_t)dt;
        sp.moved_bytes = s.bytes_moved;
        sp.moved_objects = (uint16_t)s.objects_moved;
        sp.lib_us = s.compact_time_us > 0xFFFFu
                        ? 0xFFFFu : (uint16_t)s.compact_time_us;
        sp.est_bytes = adv.estimated_moved_bytes;
        if (adv.estimated_moved_bytes == pm::COMPACTION_ESTIMATE_UNKNOWN ||
            adv.estimated_moved_objects == pm::COMPACTION_ESTIMATE_UNKNOWN) {
            est_unknown++;
        } else {
            uint32_t const dod = adv.estimated_moved_objects > s.objects_moved
                                     ? adv.estimated_moved_objects - s.objects_moved
                                     : s.objects_moved - adv.estimated_moved_objects;
            uint32_t const ddb = adv.estimated_moved_bytes > s.bytes_moved
                                     ? adv.estimated_moved_bytes - s.bytes_moved
                                     : s.bytes_moved - adv.estimated_moved_bytes;
            if (dod > max_est_objects_dev) max_est_objects_dev = dod;
            if (ddb > max_est_bytes_dev) max_est_bytes_dev = ddb;
        }
        {
            uint64_t const mc_us = dt / 1000u;
            uint64_t const lib_us = s.compact_time_us;
            uint32_t const dev = (uint32_t)(lib_us > mc_us ? lib_us - mc_us
                                                           : mc_us - lib_us);
            if (dev > max_lib_us_dev) max_lib_us_dev = dev;
        }
        if (events == 0 || s.bytes_moved < moved_bytes_min)
            moved_bytes_min = s.bytes_moved;
        if (s.bytes_moved > moved_bytes_max) moved_bytes_max = s.bytes_moved;
        moved_bytes_sum += s.bytes_moved;
        events++;

        // ---- 4. retry the demand, then refill the exact freed sizes -------
        pm::RawRef again{};
        if (pm::alloc(pool, PROBE_BYTES, 8, pm::PM_MOVABLE, 0xFFFFu, again)
            == pm::Status::Ok) {
            rescued++;
            (void)pm::free(again);
        } else {
            still_failed++;
        }
        refill_freed(k, cycles);

        if (events % 64u == 0) {
            printf("  [cw] events=%u/%u cycles=%u rescued=%u still_failed=%u"
                   " refused=%u\n", (unsigned)events, (unsigned)EVENTS,
                   (unsigned)cycles, (unsigned)rescued,
                   (unsigned)still_failed, (unsigned)compact_refused);
            std::fflush(stdout);
        }
    }

    pm::PoolStats const s_end = pm::get_stats(pool);
    bool const validate_ok = (pm::validate(pool) == pm::Status::Ok);
    for (uint32_t i = 0; i < live; ++i) (void)pm::free(refs[i]);
    (void)pm::deinit();

    // ---- report ------------------------------------------------------------
    printf("\n  compaction windows recorded: %u events in %u cycles\n",
           (unsigned)events, (unsigned)cycles);
    printf("    probe outcomes            : pre-compact ok %u, rescued by"
           " compact %u, still failed %u\n",
           (unsigned)probe_ok_pre, (unsigned)rescued, (unsigned)still_failed);
    printf("    compact refused           : %u   refill refusals: %u\n",
           (unsigned)compact_refused, (unsigned)refill_refusals);
    printf("    structure audit           : %s\n",
           validate_ok ? "OK" : "FAILED");
    if (events < 32u) {
        printf("    too few events for percentiles -- NOT reported\n");
        return events ? 0 : 1;
    }

    for (uint32_t i = 0; i < events; ++i) sorted[i] = g_samples[i].dt_ns;
    for (uint32_t i = 0; i < events; ++i) order[i] = i;
    for (uint32_t i = 1; i < events; ++i) {  // insertion sort, no allocation
        uint32_t const key = sorted[i];
        uint32_t const key_idx = order[i];
        uint32_t j = i;
        while (j > 0 && sorted[j - 1u] > key) {
            sorted[j] = sorted[j - 1u]; order[j] = order[j - 1u]; --j;
        }
        sorted[j] = key; order[j] = key_idx;
    }
    uint64_t dt_sum = 0;
    for (uint32_t i = 0; i < events; ++i) dt_sum += g_samples[i].dt_ns;

    printf("    window, ns  (nearest-rank over %u events):\n", (unsigned)events);
    print_percentile_line("ns", sorted, events, 1.0);
    printf("    min=%u  mean=%.1f  max=%u\n", (unsigned)sorted[0],
           (double)dt_sum / (double)events, (unsigned)sorted[events - 1u]);
    printf("    window, us  (same distribution, device-quotable unit):\n");
    print_percentile_line("us", sorted, events, 1000.0);
    printf("    moved bytes per event     : min %u  mean %.1f  max %u"
           "  (objects moved, last event: %u)\n",
           (unsigned)moved_bytes_min,
           (double)moved_bytes_sum / (double)events,
           (unsigned)moved_bytes_max, (unsigned)s_end.objects_moved);

    // Window vs work: the events are grouped into quintiles OF THE WINDOW
    // (Q1 = the shortest 20% of windows, Q5 = the longest). If the window is
    // dominated by relocating live objects, the moved-bytes range of each
    // quintile must rise with the quintile; if it does not, that is worth
    // investigating, not smoothing over.
    printf("    per window-quintile (Q1 shortest .. Q5 longest):"
           " mean us | moved-bytes min..max\n");
    for (uint32_t q = 0; q < 5u; ++q) {
        uint32_t const lo = q * events / 5u;
        uint32_t const hi = (q + 1u) * events / 5u;
        if (hi <= lo) continue;
        uint64_t sum = 0;
        uint32_t bmin = 0xFFFFFFFFu, bmax = 0;
        for (uint32_t i = lo; i < hi; ++i) {
            Sample const& sp = g_samples[order[i]];
            sum += sp.dt_ns;
            if (sp.moved_bytes < bmin) bmin = sp.moved_bytes;
            if (sp.moved_bytes > bmax) bmax = sp.moved_bytes;
        }
        printf("      Q%u: %10.1f | %9u..%9u\n", (unsigned)(q + 1u),
               (double)sum / (double)(hi - lo) / 1000.0,
               (unsigned)bmin, (unsigned)bmax);
    }

    // Two cross-checks, printed whether or not they flatter anyone:
    //   (a) the advice's move estimate against what compact actually moved.
    //       The policy doc claims exactness for the success case (the
    //       estimate simulates the same packing rules as the plan), and a
    //       few hundred events on a real chip is the strongest place that
    //       claim has ever been checked;
    //   (b) the library's own esp_timer microseconds against the mcycle
    //       figure -- two independent clocks must agree to within the
    //       esp_timer tick.
    printf("    advice estimate vs actual : max dev %u objects / %u bytes"
           " over %u compared (%u UNKNOWN)\n",
           (unsigned)max_est_objects_dev, (unsigned)max_est_bytes_dev,
           (unsigned)(events - est_unknown), (unsigned)est_unknown);
    printf("    library self-timing (us) vs mcycle (us): max dev %u\n",
           (unsigned)max_lib_us_dev);

    uint64_t const bias = pm_bench::interval_cost_ns();
    if (bias > 1000) {
        printf("    NOTE: one clock read on this platform costs %llu ns, so"
               " these per-event\n", (unsigned long long)bias);
        printf("    percentiles are instrument-dominated. Quotable only on"
               " the device\n    (mcycle is a register read there).\n");
    } else {
        printf("    instrument: interval bias %llu ns per event -- orders of"
               " magnitude below\n", (unsigned long long)bias);
        printf("    the median window, so the percentiles above are"
               " quotable.\n");
    }
    return 0;
}

} // namespace

// Entry point shared by the host and the on-target build; the target build
// defines PM_BENCH_NO_HOST_MAIN and calls this from app_main.
int pm_bench_compaction_window() {
    pm_bench::report("compaction window (per-event compact() duration, many events)");
    printf("PondMerge compaction-window benchmark\n");
    printf("region=%u B segment=%u population<=%u events<=%u probe=%u B"
           " K=%u..%u\n", (unsigned)REGION_BYTES, (unsigned)SEGMENT,
           (unsigned)POPULATION, (unsigned)EVENTS, (unsigned)PROBE_BYTES,
           (unsigned)PM_BENCH_CW_K_MIN, (unsigned)K_MAX);
    printf("PM_MAX_OBJECTS=%d PM_SL_COUNT=%d\n\n", (int)PM_MAX_OBJECTS,
           (int)PM_SL_COUNT);
    return run_window();
}

#ifndef PM_BENCH_NO_HOST_MAIN
int main() { return pm_bench_compaction_window(); }
#endif
