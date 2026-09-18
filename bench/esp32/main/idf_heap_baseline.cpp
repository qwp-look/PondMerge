// The independent baseline: the allocator ESP-IDF itself ships.
//
// =============================================================================
// IT IS NOT heap_4 -- AND THE FIRST RUN OF THIS FILE PROVED IT THE HARD WAY
// =============================================================================
// This file was written as "the FreeRTOS heap_4 baseline", because that is what
// ESP-IDF's heap used to be. IDF v6.0.2 has no heap_4 at all: the only allocator
// implementation in components/heap is components/heap/tlsf/tlsf.c. The evidence
// is not a config symbol, it is this, from the first on-target run:
//
//     assert failed: tlsf_free tlsf.c:630 (!block_is_free(block) && "block
//     already marked as free")
//
// That was a real bug in this file (see the churn loop below, where a failed
// same-size re-allocation used to leave a dangling pointer behind), but the
// function that caught it names the allocator. So the baseline is TLSF.
//
// This makes the comparison better, not worse. PondMerge's allocator is
// TLSF-style -- segregated fit with a bitmap index -- and TLSF is mature,
// widely deployed, and not written here. The baseline is therefore a mature
// production implementation of the same algorithmic family, which is a far more
// demanding thing to be measured against than a strawman would be.
//
// =============================================================================
// WHY THERE IS NO HOST COUNTERPART
// =============================================================================
// The value of this file is that the baseline is SOMEBODY ELSE'S allocator.
// There is no equivalent third party inside the host build, so the host side
// only does the compaction on/off A/B (bench/fragmentation.cpp) and this
// comparison happens on the target, where a real embedded allocator is present.
//
// =============================================================================
// HOW THE REGION IS OBTAINED
// =============================================================================
// The part has one 192 KiB region to spare, not two, so the SAME array the
// PondMerge benchmarks used (g_zone, owned by bench_main.cpp) is handed to the
// IDF heap once PondMerge has been deinit'ed. bench_main.cpp runs this last for
// exactly that reason. heap_caps_add_region_with_caps() registers it with a
// custom capability bit so the measurements below see THIS region and not the
// IDF heap sitting alongside it -- without that bit,
// heap_caps_get_largest_free_block() would report the IDF heap's free block and
// hide everything this file is trying to measure.
//
// Honest caveat about that bit: the region also carries MALLOC_CAP_8BIT, which
// it must in order to serve byte allocations, so it simultaneously joins the
// default 8-bit heap and IDF's own dynamic allocations may land in it. The
// accounting line printed at the end reads the region back through
// heap_caps_get_info(), so any such interference is visible rather than assumed
// away.
//
// =============================================================================
// WHAT IS AND IS NOT COMPARABLE
// =============================================================================
// Same workload shape, same fixed seed, same ordering rules, same constants as
// bench/fragmentation.cpp -- and they come from the same CMake variables, so the
// two cannot drift apart: fill the region with the steep size mix, free every
// SCATTER_MOD-th object, then issue the same PROBE_BYTES demands back to back
// (phase 1) and under churn (phase 2).
//
// One difference is REAL and is not corrected for, because it is the
// allocator's own property: the baseline spends its own per-allocation
// bookkeeping, so the same region holds a different number of objects. The fill
// count is reported so the reader can see it rather than be told the two are
// identical -- and it is a result in its own right.
//
// The baseline never relocates and cannot be asked to consolidate, so there is
// one baseline variant and not two. That is the entire point of the comparison:
// PondMerge-B is the variant that consolidates.
//
// "Pinned" has no meaning here for the same reason -- nothing moves, so no
// object needs protecting from a move. The fence posts that sustain
// fragmentation for PondMerge are, for the baseline, simply the objects that the
// scatter phase does not free.

#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"

#include <cstdint>
#include <cstdio>

// Owned by bench_main.cpp; see the note above on why it is shared.
extern uint8_t g_zone[];

namespace {

// The constants are the same ones bench/fragmentation.cpp uses, and the device
// CMake passes them to both files. The fallbacks here match that file's, so a
// host-side change to the defaults shows up as a mismatch on the device rather
// than as a silent divergence.
#if !defined(PM_BENCH_ZONE_BYTES)
#error "PM_BENCH_ZONE_BYTES must come from the build"
#endif
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
#define PM_BENCH_PINNED_EVERY 16u
#endif
#ifndef PM_BENCH_SCATTER_MOD
#define PM_BENCH_SCATTER_MOD 4u
#endif
#ifndef PM_BENCH_PHASE1_TRIALS
#define PM_BENCH_PHASE1_TRIALS 50u
#endif

constexpr uint32_t REGION_BYTES  = PM_BENCH_ZONE_BYTES;
constexpr uint32_t POPULATION    = PM_BENCH_POPULATION;
constexpr uint32_t PROBE_BYTES   = PM_BENCH_PROBE_BYTES;
constexpr uint32_t STEPS         = PM_BENCH_STEPS;
constexpr uint32_t PROBE_EVERY   = PM_BENCH_PROBE_EVERY;
constexpr uint32_t PINNED_EVERY  = PM_BENCH_PINNED_EVERY;
constexpr uint32_t SCATTER_MOD   = PM_BENCH_SCATTER_MOD;
constexpr uint32_t PHASE1_TRIALS = PM_BENCH_PHASE1_TRIALS;
constexpr uint32_t ALIGN         = 8u;

// A capability bit that no IDF heap uses, so that this region can be addressed
// on its own. Bits 0..20 are taken (see esp_heap_caps.h), bit 31 is INVALID.
//
// Two spellings of the same thing on purpose: the region registration API takes
// an ordered ARRAY of capability masks, and everything else takes the MASK.
constexpr uint32_t kRegionCap  = 1u << 21;
constexpr uint32_t kRegionCaps = MALLOC_CAP_8BIT | kRegionCap;
const uint32_t kCapsArray[] = {MALLOC_CAP_8BIT, kRegionCap, 0};

static_assert(POPULATION >= 2, "need at least two objects to scatter holes");

const uint32_t kSizes[] = {256, 512, 768, 1024, 1536, 2048};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

uint32_t g_rng = 0xB5297A4Du;
inline uint32_t rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

void* *g_refs = nullptr;      // sized POPULATION, taken from the region's head
uint32_t* g_sizes = nullptr;

struct Report {
    uint32_t region_bytes = 0;
    uint32_t added_ok = 0;
    uint32_t filled = 0;
    uint32_t keepers = 0;
    uint32_t live_after_scatter = 0;
    uint32_t p1_probes = 0;
    uint32_t p1_fail = 0;
    uint32_t p2_probes = 0;
    uint32_t p2_fail = 0;
    uint32_t min_largest = 0xFFFFFFFFu;
    uint32_t final_largest = 0;
    uint32_t churn_refusals = 0;
    // The allocator's own accounting, snapshotted BEFORE the cleanup below --
    // after the cleanup everything is free and the largest block is legitimately
    // the whole region, so comparing that against a figure taken during the churn
    // is comparing two different moments and always reports a mismatch. (It did,
    // in the first version of this check.)
    uint32_t acct_free = 0;
    uint32_t acct_alloc = 0;
    uint32_t acct_largest = 0;
};

Report run_baseline() {
    Report r;

    // The bookkeeping arrays live at the head of g_zone, OUTSIDE the range that
    // is registered as a heap, so the allocator sees a clean 192 KiB minus only
    // what is genuinely needed to track the results.
    uint32_t const bookkeeping =
        POPULATION * (uint32_t)(sizeof(void*) + sizeof(uint32_t));
    uint8_t* const base = g_zone + bookkeeping;
    uint32_t const region = REGION_BYTES - bookkeeping;
    g_refs = reinterpret_cast<void**>(g_zone);
    g_sizes = reinterpret_cast<uint32_t*>(g_zone + POPULATION * sizeof(void*));

    esp_err_t const added =
        heap_caps_add_region_with_caps(kCapsArray, (intptr_t)base,
                                       (intptr_t)(base + region));
    std::printf("  heap_caps_add_region_with_caps -> %s (region %u B at %p)\n",
                esp_err_to_name(added), (unsigned)region, (void*)base);
    if (added != ESP_OK) return r;
    r.added_ok = 1;
    r.region_bytes = region;

    uint32_t live = 0;
    g_rng = 0xB5297A4Du;
    for (uint32_t i = 0; i < POPULATION; ++i) {
        uint32_t const size = kSizes[i % kSizeCount];
        bool const keeper = ((i + 1) % PINNED_EVERY) == 0;
        void* p = heap_caps_aligned_alloc(ALIGN, size, kRegionCaps);
        if (p == nullptr) break;  // region exhausted, same stopping rule
        g_refs[live] = p;
        g_sizes[live] = size;
        if (keeper) r.keepers++;
        live++;
    }
    r.filled = live;
    std::printf("  %-32s filled=%u (keepers=%u)\n", "baseline", (unsigned)live,
                (unsigned)r.keepers);
    if (live == 0) return r;

    // scatter: free every SCATTER_MOD-th slot, leaving isolated holes fenced in
    // by live neighbours -- identical indexing rule to fragmentation.cpp.
    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < live; ++i) {
            if (((i + 1) % SCATTER_MOD) == 0) {
                heap_caps_free(g_refs[i]);
                continue;
            }
            g_refs[w] = g_refs[i];
            g_sizes[w] = g_sizes[i];
            w++;
        }
        live = w;
    }
    r.live_after_scatter = live;
    std::printf("  %-32s after scatter: live=%u\n", "baseline", (unsigned)live);

    auto largest = [&]() -> uint32_t {
        return (uint32_t)heap_caps_get_largest_free_block(kRegionCaps);
    };
    r.final_largest = largest();
    r.min_largest = r.final_largest;

    auto demand = [&]() -> bool {
        void* p = heap_caps_aligned_alloc(ALIGN, PROBE_BYTES, kRegionCaps);
        if (p == nullptr) return false;
        heap_caps_free(p);
        return true;
    };

    // phase 1: the fragmented STATE, demands back to back, no churn.
    for (uint32_t t = 0; t < PHASE1_TRIALS; ++t) {
        r.p1_probes++;
        if (!demand()) r.p1_fail++;
        uint32_t const lf = largest();
        if (lf < r.min_largest) r.min_largest = lf;
    }
    r.final_largest = largest();

    // phase 2: the same demand under sustained churn.
    //
    // Fairness rule 3 in fragmentation.cpp says the churn re-allocates the size
    // it just freed "so it cannot be refused by construction". That claim is
    // TRUE for the allocator it was written for and was verified there, but it
    // is not a law of allocators, and this loop must survive the case where it
    // does not hold -- freeing a slot and then failing to replace it must not
    // leave the slot pointing at freed memory, or the next visit to that slot
    // double-frees and the heap aborts. (The first version of this file did
    // exactly that, which is how the tlsf_free assertion in the header comment
    // came about.) The failing slot is therefore DROPPED from the tracked set
    // and the refusal is counted, so the run is flagged rather than corrupted.
    for (uint32_t step = 0; step < STEPS && live > 1; ++step) {
        uint32_t const i = rng() % live;
        uint32_t const size = g_sizes[i];
        heap_caps_free(g_refs[i]);
        void* p = heap_caps_aligned_alloc(ALIGN, size, kRegionCaps);
        if (p == nullptr) {
            r.churn_refusals++;
            live--;
            g_refs[i] = g_refs[live];
            g_sizes[i] = g_sizes[live];
            continue;
        }
        g_refs[i] = p;

        if (step % PROBE_EVERY != 0) continue;
        r.p2_probes++;
        if (!demand()) r.p2_fail++;
        uint32_t const lf = largest();
        if (lf < r.min_largest) r.min_largest = lf;
        r.final_largest = lf;
    }

    // Both readings taken at the SAME instant. The running minimum above is a
    // minimum over the whole load, but "final" and the allocator's own view have
    // to describe one moment or the cross-check is comparing two different
    // states and will report a mismatch that means nothing. (It did, twice.)
    r.final_largest = largest();
    {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, kRegionCaps);
        r.acct_free = (uint32_t)info.total_free_bytes;
        r.acct_alloc = (uint32_t)info.total_allocated_bytes;
        r.acct_largest = (uint32_t)info.largest_free_block;
    }

    for (uint32_t i = 0; i < live; ++i) heap_caps_free(g_refs[i]);
    return r;
}

} // namespace

int pm_bench_idf_heap_baseline() {
    std::printf("\n--- independent baseline ---------------------------------------\n");
    std::printf("ESP-IDF's own heap over the same %u B region. IDF v6.0.2 has no\n",
                (unsigned)REGION_BYTES);
    std::printf("heap_4: components/heap/tlsf/tlsf.c is the allocator, i.e. TLSF --\n");
    std::printf("a mature production implementation of the same segregated-fit family\n");
    std::printf("as PondMerge. It never relocates, so there is one variant.\n");
    std::printf("region=%u B population<=%u steps=%u probe=%u B/%u steps\n",
                (unsigned)REGION_BYTES, (unsigned)POPULATION, (unsigned)STEPS,
                (unsigned)PROBE_BYTES, (unsigned)PROBE_EVERY);
    std::printf("scatter_mod=%u keepers_every=%u alignment=%u\n",
                (unsigned)SCATTER_MOD, (unsigned)PINNED_EVERY, (unsigned)ALIGN);

    Report const r = run_baseline();

    std::printf("\n  headline (the same two numbers fragmentation.cpp reports)\n");
    std::printf("    phase 1 (fragmented state, no churn): %u of %u demands FAILED\n",
                (unsigned)r.p1_fail, (unsigned)r.p1_probes);
    std::printf("    phase 2 (sustained churn)          : %u of %u demands FAILED\n",
                (unsigned)r.p2_fail, (unsigned)r.p2_probes);
    std::printf("    largest free block                 : min %u B, final %u B\n",
                (unsigned)r.min_largest, (unsigned)r.final_largest);
    std::printf("    churn alloc refusals               : %u%s\n",
                (unsigned)r.churn_refusals,
                r.churn_refusals ? "  *** CONFOUNDED RUN ***" : "");
    if (r.churn_refusals) {
        std::printf("      (same-size re-allocation was refused %u times. That is a"
                    " real property of\n      this allocator under this load, and"
                    " it is also why each refusal drops its\n      slot from the"
                    " tracked set -- see the loop. Do not quote the two failure\n"
                    "      counts without it.)\n", (unsigned)r.churn_refusals);
    }
    std::printf("    no compaction and no structure audit: nothing to consolidate"
                " with, so there\n    is one baseline column and no second one.\n");

    // Accounting cross-check in place of validate(): the region's own numbers
    // must add up, otherwise the failure counts above are meaningless. Both
    // figures come from the same moment, just before the cleanup.
    std::printf("    accounting at the end of the load: free %u + allocated %u = %u"
                " (region %u)\n",
                (unsigned)r.acct_free, (unsigned)r.acct_alloc,
                (unsigned)(r.acct_free + r.acct_alloc), (unsigned)r.region_bytes);
    std::printf("    largest free block, allocator's own view: %u B vs %u B"
                " measured -> %s\n",
                (unsigned)r.acct_largest, (unsigned)r.final_largest,
                r.acct_largest == r.final_largest
                    ? "consistent"
                    : "MISMATCH (foreign allocator use, or a bug here)");
    if (r.acct_largest > r.region_bytes) {
        std::printf("    (the allocator reports more free bytes than the region"
                    " contains: the check\n     is wrong, not the allocator.)\n");
    }
    std::printf("-------------------------------------------------------------------------\n");
    return r.churn_refusals ? 1 : 0;
}
