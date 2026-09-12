// PondMerge allocator hot-path micro-benchmark.
//
// Purpose: determine whether alloc's documented O(bin chain + live_objects)
// bound is a real cost in practice, and which of the two terms dominates.
// NOT a fragmentation benchmark -- that is a separate, larger experiment.
//
// Method:
//   Phase 1 (fill): allocate N objects with a mixed-size cycle, timing every
//     single alloc. Because live count grows monotonically from 0 to N, the
//     elapsed time per alloc bucketed by live count IS the scaling curve for
//     alloc, with no confounds.
//   Phase 2 (steady state): at full live count, repeatedly free a RANDOM live
//     object and re-allocate the same size, timing free and alloc separately.
//     Random position is deliberate: a fixed position would always insert at
//     the same place in the address-ordered list and understate the walk.
#include "pondmerge/pondmerge.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

constexpr uint32_t ZONE_BYTES   = 256u * 1024u;
constexpr uint32_t SEGMENT      = 4096u;
constexpr uint32_t TARGET_LIVE  = 1024u;
constexpr uint32_t STEADY_OPS   = 200000u;
constexpr uint32_t BUCKET       = 64u;

alignas(16) uint8_t g_zone[ZONE_BYTES];

const uint32_t kSizes[] = {32, 48, 64, 96, 128, 192, 256};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

struct Slot { pm::RawRef ref; uint32_t size; };

Slot g_slots[TARGET_LIVE];
uint32_t g_live = 0;

uint32_t g_rng = 0x9E3779B9u;
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

} // namespace

int main() {
    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) { printf("init failed\n"); return 1; }
    pm::PoolId pool{};
    if (pm::create_pool(pool, ZONE_BYTES / SEGMENT) != pm::Status::Ok) {
        printf("create_pool failed\n"); return 1;
    }
    printf("PM_MAX_OBJECTS=%d PM_SL_COUNT=%d PM_FL_MAX=%d\n",
           (int)PM_MAX_OBJECTS, (int)PM_SL_COUNT, (int)PM_FL_MAX);
    printf("zone=%u segment=%u pool_capacity=%u\n",
           (unsigned)ZONE_BYTES, (unsigned)SEGMENT,
           (unsigned)((ZONE_BYTES / SEGMENT) * SEGMENT));

    // ---------------- Phase 1: alloc cost vs live count ----------------
    uint64_t bucket_ns[TARGET_LIVE / BUCKET + 1] = {};
    uint32_t bucket_n[TARGET_LIVE / BUCKET + 1] = {};

    for (uint32_t i = 0; i < TARGET_LIVE; ++i) {
        uint32_t sz = kSizes[i % kSizeCount];
        pm::RawRef r{};
        uint64_t t0 = now_ns();
        pm::Status st = pm::alloc(pool, sz, 8, pm::PM_MOVABLE, i, r);
        uint64_t dt = now_ns() - t0;
        if (st != pm::Status::Ok) {
            printf("fill stopped at live=%u status=%s\n", (unsigned)i,
                   pm::status_name(st));
            break;
        }
        uint32_t b = g_live / BUCKET;
        bucket_ns[b] += dt;
        bucket_n[b]++;
        g_slots[g_live].ref = r;
        g_slots[g_live].size = sz;
        g_live++;
    }
    printf("\n--- phase 1: alloc latency vs live count (fill cost) ---\n");
    printf("%-14s %8s %12s\n", "live range", "samples", "avg alloc");
    for (uint32_t b = 0; b * BUCKET < g_live; ++b) {
        if (bucket_n[b] == 0) continue;
        char range[32];
        snprintf(range, sizeof(range), "%u-%u", b * BUCKET,
                 (b + 1) * BUCKET - 1);
        printf("%-14s %8u %9.1f ns\n", range, (unsigned)bucket_n[b],
               (double)bucket_ns[b] / (double)bucket_n[b]);
    }
    printf("filled live=%u\n", (unsigned)g_live);

    uint64_t fill_ns = 0;
    for (uint32_t b = 0; b * BUCKET < g_live; ++b) fill_ns += bucket_ns[b];
    uint32_t fill_n = 0;
    for (uint32_t b = 0; b * BUCKET < g_live; ++b) fill_n += bucket_n[b];
    if (fill_n) printf("fill average: %.1f ns/alloc\n",
                       (double)fill_ns / (double)fill_n);

    pm::PoolStats s0 = pm::get_stats(pool);
    printf("largest_free_block=%u free_bytes=%u fragment=%u\n",
           (unsigned)s0.largest_free_block, (unsigned)s0.free_bytes,
           (unsigned)s0.fragment_bytes);

    // ---------------- Phase 2: steady state -------------------------------
    uint64_t free_ns = 0, alloc_ns = 0;
    uint32_t ops = 0, alloc_fail = 0;
    uint32_t peak_live = g_live;
    for (uint32_t k = 0; k < STEADY_OPS && g_live > 1; ++k) {
        uint32_t i = rng() % g_live;
        uint32_t sz = g_slots[i].size;
        pm::RawRef old = g_slots[i].ref;
        uint64_t t0 = now_ns();
        pm::Status fs = pm::free(old);
        uint64_t t1 = now_ns();
        if (fs != pm::Status::Ok) { printf("free fail %s\n", pm::status_name(fs)); break; }
        free_ns += t1 - t0;

        pm::RawRef nr{};
        uint64_t t2 = now_ns();
        pm::Status as = pm::alloc(pool, sz, 8, pm::PM_MOVABLE, i, nr);
        uint64_t t3 = now_ns();
        if (as != pm::Status::Ok) { alloc_fail++; g_slots[i].ref = pm::RawRef{}; continue; }
        alloc_ns += t3 - t2;
        g_slots[i].ref = nr;
        ops++;
    }
    printf("\n--- phase 2: steady state, random position, live=%u ---\n",
           (unsigned)g_live);
    printf("ops=%u alloc_fail=%u peak_live=%u\n", (unsigned)ops,
           (unsigned)alloc_fail, (unsigned)peak_live);
    if (ops) {
        printf("free  avg: %8.1f ns\n", (double)free_ns / (double)ops);
        printf("alloc avg: %8.1f ns\n", (double)alloc_ns / (double)ops);
        printf("pair  avg: %8.1f ns\n",
               ((double)free_ns + (double)alloc_ns) / (double)ops);
    }

    pm::Status v = pm::validate(pool);
    printf("validate: %s\n", pm::status_name(v));
    pm::GlobalStats g = pm::global_stats();
    printf("metadata_bytes=%u max_live=%u\n", (unsigned)g.metadata_bytes,
           (unsigned)g.max_live_objects);
    return 0;
}
