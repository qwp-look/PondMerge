// PondMerge fragmentation benchmark -- does explicit compaction actually
// prevent fragmentation-induced allocation failure?
//
// ---------------------------------------------------------------------------
// WHY THIS FILE HAS NO HAND-WRITTEN "SIMPLE ALLOCATOR" TO WIN AGAINST
// ---------------------------------------------------------------------------
// Comparing PondMerge against an allocator we wrote ourselves would be
// self-serving, so the decisive experiment here is a controlled A/B on the SAME
// allocator, over the SAME workload, on the SAME region:
//
//   variant A  compaction disabled
//   variant B  compaction triggered ONLY by a failed probe allocation
//
// The comparison against an INDEPENDENT allocator happens on the ESP32 build
// against FreeRTOS heap_4 (ESP-IDF's default) over an equally sized dedicated
// region -- a third-party baseline rather than our own. See bench/README.md.
//
// ---------------------------------------------------------------------------
// THE WORKLOAD, AND WHY IT LOOKS LIKE THIS
// ---------------------------------------------------------------------------
// Two earlier shapes of this benchmark were WRONG and are worth recording:
//
//   (1) A narrow size mix (64..384 B) with uniform churn produced ZERO probe
//       failures: free() coalesces adjacent free blocks, so uniform churn keeps
//       the free space consolidated. No fragmentation, nothing to measure.
//   (2) A steep mix alone still healed itself, because first-fit allocations
//       drift towards low addresses while free space accumulates at the high
//       end -- the allocator defragments by itself over time.
//
// What actually sustains fragmentation is PINNED objects: they are relocation
// barriers, so free space between them can never be consolidated by a plain
// free(), and eventually not even by compaction. That is not a trick invented
// for this benchmark -- pinned IS what DMA buffers are, and it is a core
// PondMerge concept. So:
//
//   phase 1  fill the region to exhaustion with a steep size mix, allocating
//            every PINNED_EVERY-th object as pinned (the fence posts)
//   phase 2  release every SCATTER_MOD-th *movable* object, leaving holes
//            fenced in by live and pinned neighbours
//   phase 3  churn + probe: free a movable object and re-allocate the SAME
//            size, and every PROBE_EVERY steps demand one large contiguous
//            block
//
// FAIRNESS RULES (see bench/README.md; a run that breaks any of these is
// reported as CONFOUNDED and must not be quoted):
//   1. Compaction is NEVER triggered from the churn path, only by a failed
//      probe. Otherwise a compacting variant could retain a population the
//      other one loses, and the two would no longer share a workload.
//   2. Both variants draw from one fixed-seed PRNG in the same order.
//   3. The churn re-allocates the size it just freed, so it cannot be refused
//      by construction. `churn_refusals` is still reported, and a non-zero
//      value marks the run confounded.
//
// Build (host):
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc bench/fragmentation.cpp src/core.cpp -o build/bench_frag
//   ./build/bench_frag

#include "pondmerge/pondmerge.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

#ifndef PM_BENCH_POPULATION
#define PM_BENCH_POPULATION 256u
#endif
#ifndef PM_BENCH_PROBE_BYTES
#define PM_BENCH_PROBE_BYTES (12u * 1024u)
#endif
#ifndef PM_BENCH_STEPS
#define PM_BENCH_STEPS 100000u
#endif
#ifndef PM_BENCH_PROBE_EVERY
#define PM_BENCH_PROBE_EVERY 200u
#endif
#ifndef PM_BENCH_PINNED_EVERY
#define PM_BENCH_PINNED_EVERY 16u   // every Nth object is a pinned fence post
#endif
#ifndef PM_BENCH_SCATTER_MOD
#define PM_BENCH_SCATTER_MOD 4u     // release every Nth movable object
#endif
#ifndef PM_BENCH_PHASE1_TRIALS
#define PM_BENCH_PHASE1_TRIALS 50u  // back-to-back demands, no churn between
#endif

constexpr uint32_t REGION_BYTES  = 256u * 1024u;
constexpr uint32_t SEGMENT       = 4096u;
constexpr uint32_t POPULATION    = PM_BENCH_POPULATION;
constexpr uint32_t STEPS         = PM_BENCH_STEPS;
constexpr uint32_t PROBE_EVERY   = PM_BENCH_PROBE_EVERY;
constexpr uint32_t PROBE_BYTES   = PM_BENCH_PROBE_BYTES;
constexpr uint32_t PINNED_EVERY  = PM_BENCH_PINNED_EVERY;
constexpr uint32_t SCATTER_MOD   = PM_BENCH_SCATTER_MOD;
constexpr uint32_t PHASE1_TRIALS = PM_BENCH_PHASE1_TRIALS;

static_assert(POPULATION <= PM_MAX_OBJECTS,
              "the population must fit in the descriptor table");
static_assert(PINNED_EVERY >= 2, "PINNED_EVERY=1 would pin everything");

// Steep size mix: a freed 256 B slot cannot serve a later 2 KiB request, and
// large sizes are needed anyway to reach high utilisation under a
// PM_MAX_OBJECTS cap.
const uint32_t kSizes[] = {256, 512, 768, 1024, 1536, 2048};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

alignas(16) uint8_t g_zone[REGION_BYTES];

uint32_t g_rng;
inline uint32_t rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

using clock_t_ = std::chrono::steady_clock;
inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock_t_::now().time_since_epoch())
        .count();
}

struct Report {
    const char* name = "";
    uint32_t filled = 0;
    uint32_t pinned = 0;
    uint32_t live_after_scatter = 0;
    // Phase I: is the fragmented state serviceable at all? Demands are issued
    // back to back with no churn in between, so this measures the STATE.
    uint32_t p1_probes = 0;
    uint32_t p1_fail = 0;
    uint32_t p1_rescued = 0;
    // Phase II: the same demand under sustained churn -- a RATE, not a state.
    uint32_t p2_probes = 0;
    uint32_t p2_fail = 0;
    uint32_t p2_rescued = 0;
    uint32_t compactions = 0;
    uint32_t compact_refused = 0;
    uint64_t compact_ns_total = 0;
    uint64_t compact_ns_worst = 0;
    uint32_t min_largest = 0xFFFFFFFFu;
    uint32_t final_largest = 0;
    uint32_t moved_objects = 0;
    uint32_t moved_bytes = 0;
    uint32_t churn_refusals = 0;
    uint64_t wall_ns = 0;
    bool validate_ok = false;
};

Report run_pondmerge(const char* name, bool compact_on_failure) {
    Report r;
    r.name = name;

    static pm::RawRef refs[POPULATION];
    static uint32_t sizes[POPULATION];
    uint32_t live = 0;

    pm::Config cfg{g_zone, REGION_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) { printf("init failed\n"); return r; }
    pm::PoolId pool{};
    if (pm::create_pool(pool, REGION_BYTES / SEGMENT) != pm::Status::Ok) {
        printf("create_pool failed\n"); return r;
    }

    // ---- phase 1: fill to exhaustion, with pinned fence posts -------------
    g_rng = 0xB5297A4Du;
    for (uint32_t i = 0; i < POPULATION; ++i) {
        uint32_t const size = kSizes[i % kSizeCount];
        bool const pin = ((i + 1) % PINNED_EVERY) == 0;
        pm::RawRef ref{};
        pm::Status st = pm::alloc(pool, size, 8,
                                  pin ? uint16_t(pm::PM_PINNED)
                                      : uint16_t(pm::PM_MOVABLE),
                                  i, ref);
        if (st != pm::Status::Ok) break; // region exhausted
        refs[live] = ref;
        sizes[live] = size;
        if (pin) r.pinned++;
        live++;
    }
    r.filled = live;
    printf("  %-32s filled=%u (pinned=%u)\n", name, (unsigned)live,
           (unsigned)r.pinned);

    // ---- phase 2: scatter holes, fenced in by live + pinned neighbours ----
    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < live; ++i) {
            // The movable objects are appended after the pinned ones in fill
            // order, but a simple "every Nth slot" is enough: PID ordering does
            // not matter here, only that released blocks are separated.
            if (((i + 1) % SCATTER_MOD) == 0) {
                (void)pm::free(refs[i]);
                continue;
            }
            refs[w] = refs[i];
            sizes[w] = sizes[i];
            w++;
        }
        live = w;
    }
    r.live_after_scatter = live;
    printf("  %-32s after scatter: live=%u\n", name, (unsigned)live);

    pm::CompactionRequest req{PROBE_BYTES, 8, 0, 0};

    // One large contiguous demand, with the SAME compaction policy in both
    // phases. Returns whether the demand was eventually satisfied.
    auto demand_probe = [&](uint32_t* probes, uint32_t* fails,
                            uint32_t* rescued) -> bool {
        (*probes)++;
        pm::RawRef probe{};
        if (pm::alloc(pool, PROBE_BYTES, 8, pm::PM_MOVABLE, 0xFFFFu, probe)
            == pm::Status::Ok) {
            (void)pm::free(probe);
            return true;
        }
        (*fails)++;
        if (!compact_on_failure) return false;

        // Read-only advice first, as a product would do.
        (void)pm::analyze_compaction(pool, &req);
        uint64_t const c0 = now_ns();
        pm::Status cs = pm::compact(pool);
        uint64_t const dt = now_ns() - c0;
        if (cs != pm::Status::Ok) {
            r.compact_refused++;
            (void)pm::resume(pool); // a refused compact stays Paused
            return false;
        }
        r.compactions++;
        r.compact_ns_total += dt;
        if (dt > r.compact_ns_worst) r.compact_ns_worst = dt;

        pm::RawRef again{};
        if (pm::alloc(pool, PROBE_BYTES, 8, pm::PM_MOVABLE, 0xFFFFu, again)
            != pm::Status::Ok)
            return false;
        (*rescued)++;
        (void)pm::free(again);
        return true;
    };

    auto sample_largest = [&]() {
        pm::PoolStats s = pm::get_stats(pool);
        if (s.valid && s.largest_free_block < r.min_largest)
            r.min_largest = s.largest_free_block;
        r.final_largest = s.largest_free_block;
    };

    uint64_t const t_start = now_ns();

    // ---- phase 1: is the fragmented state serviceable at all? -------------
    // Demands back to back with NO churn between them. This measures the STATE
    // that the scatter step produced, not a rate -- and that is deliberate: for
    // variant B a single compaction restores serviceability, so the remaining
    // trials succeeding is not a trick, it IS the claim under test.
    sample_largest();
    for (uint32_t t = 0; t < PHASE1_TRIALS; ++t)
        (void)demand_probe(&r.p1_probes, &r.p1_fail, &r.p1_rescued);
    sample_largest();

    // ---- phase 2: the same demand under sustained churn -------------------
    for (uint32_t step = 0; step < STEPS && live > 1; ++step) {
        // Churn: free a random live slot and re-allocate ITS OWN size. A
        // same-size re-allocation cannot be refused, which keeps both variants
        // on an identical workload (fairness rule 3).
        uint32_t const i = rng() % live;
        uint32_t const size = sizes[i];
        if (pm::free(refs[i]) != pm::Status::Ok) continue;
        pm::RawRef nr{};
        if (pm::alloc(pool, size, 8, pm::PM_MOVABLE, step, nr) != pm::Status::Ok) {
            r.churn_refusals++;
            continue;
        }
        refs[i] = nr;

        if (step % PROBE_EVERY != 0) continue;
        (void)demand_probe(&r.p2_probes, &r.p2_fail, &r.p2_rescued);
        sample_largest();
    }

    r.wall_ns = now_ns() - t_start;
    pm::PoolStats s = pm::get_stats(pool);
    r.moved_objects = s.objects_moved;
    r.moved_bytes = s.bytes_moved;
    r.validate_ok = (pm::validate(pool) == pm::Status::Ok);
    if (r.min_largest == 0xFFFFFFFFu) r.min_largest = 0;

    for (uint32_t i = 0; i < live; ++i) (void)pm::free(refs[i]);
    (void)pm::deinit();
    return r;
}

void print_report(const Report& r) {
    printf("\n  %s\n", r.name);
    printf("    [phase 1] fragmented state, no churn between demands\n");
    printf("      demands %u of %u B  ->  FAILURES %u, recovered by compact %u\n",
           (unsigned)r.p1_probes, (unsigned)PROBE_BYTES, (unsigned)r.p1_fail,
           (unsigned)r.p1_rescued);
    printf("    [phase 2] same demand under sustained churn\n");
    printf("      demands %u of %u B  ->  FAILURES %u, recovered by compact %u\n",
           (unsigned)r.p2_probes, (unsigned)PROBE_BYTES, (unsigned)r.p2_fail,
           (unsigned)r.p2_rescued);
    printf("    compactions              : %u ok, %u refused\n",
           (unsigned)r.compactions, (unsigned)r.compact_refused);
    if (r.compactions) {
        printf("    compact time total       : %.3f ms (worst %.3f ms)\n",
               (double)r.compact_ns_total / 1e6,
               (double)r.compact_ns_worst / 1e6);
        printf("    last compact moved       : %u objects / %u bytes\n",
               (unsigned)r.moved_objects, (unsigned)r.moved_bytes);
    }
    printf("    largest free block       : min %u B, final %u B\n",
           (unsigned)r.min_largest, (unsigned)r.final_largest);
    printf("    churn alloc refusals     : %u%s\n", (unsigned)r.churn_refusals,
           r.churn_refusals ? "  *** CONFOUNDED RUN ***" : "");
    printf("    structure audit          : %s\n",
           r.validate_ok ? "OK" : "FAILED");
    printf("    wall time                : %.1f ms\n", (double)r.wall_ns / 1e6);
}

} // namespace

int main() {
    printf("PondMerge fragmentation benchmark\n");
    printf("region=%u B segment=%u population<=%u steps=%u probe=%u B/%u steps\n",
           (unsigned)REGION_BYTES, (unsigned)SEGMENT, (unsigned)POPULATION,
           (unsigned)STEPS, (unsigned)PROBE_BYTES, (unsigned)PROBE_EVERY);
    printf("pinned_every=%u scatter_mod=%u PM_MAX_OBJECTS=%d PM_SL_COUNT=%d\n\n",
           (unsigned)PINNED_EVERY, (unsigned)SCATTER_MOD, (int)PM_MAX_OBJECTS,
           (int)PM_SL_COUNT);

    Report a = run_pondmerge("variant A: compaction disabled", false);
    Report b = run_pondmerge("variant B: compact on probe failure", true);

    print_report(a);
    print_report(b);

    printf("\n--- summary ---\n");
    printf("  phase 1 (fragmented state, no churn):\n");
    printf("    compaction disabled -> %u of %u demands FAILED\n",
           (unsigned)a.p1_fail, (unsigned)a.p1_probes);
    printf("    compaction enabled  -> %u of %u demands FAILED\n",
           (unsigned)b.p1_fail, (unsigned)b.p1_probes);
    printf("  phase 2 (sustained churn):\n");
    printf("    compaction disabled -> %u of %u demands FAILED\n",
           (unsigned)a.p2_fail, (unsigned)a.p2_probes);
    printf("    compaction enabled  -> %u of %u demands FAILED\n",
           (unsigned)b.p2_fail, (unsigned)b.p2_probes);
    if (!a.p1_fail && !a.p2_fail) {
        printf("  NOTE: variant A never failed -- the workload does not fragment\n"
               "        at these settings; lower PINNED_EVERY or raise PROBE_BYTES.\n");
    }
    printf("  smallest largest-free-block seen: A=%u B=%u bytes\n",
           (unsigned)a.min_largest, (unsigned)b.min_largest);
    if (b.compactions) {
        printf("  cost: %u compactions, %.3f ms total, avg %.1f us, worst %.3f ms\n",
               (unsigned)b.compactions, (double)b.compact_ns_total / 1e6,
               (double)b.compact_ns_total / (double)b.compactions / 1e3,
               (double)b.compact_ns_worst / 1e6);
    } else if (a.p1_fail || a.p2_fail) {
        printf("  compaction was never even attempted or never succeeded (%u refusals)\n",
               (unsigned)b.compact_refused);
    }
    if (a.churn_refusals || b.churn_refusals) {
        printf("  WARNING: churn refusals present -> CONFOUNDED, do not quote.\n");
    }
    return 0;
}
