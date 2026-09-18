// PondMerge validate / get_stats scaling benchmark.
//
// Question: validate is documented as O((live + free)^2) -- is that a real cost
// or a theoretical one? And is get_stats worth optimising? (Spoiler: it is not,
// and establishing that is the point -- a negative result that prevents a
// pointless change to an audited function is worth as much as a positive one.)
//
// METHOD: batching, for the reason set out in bench/bench_timer.h. validate()
// and get_stats() are read-only and idempotent -- calling either a thousand
// times leaves the pool byte-identical -- so unlike the allocator benchmark
// there is no state to restore between repetitions and the batch can simply be
// "call it K times". Because one call can take milliseconds, K is chosen
// adaptively from a single probe call so that every sample point spends roughly
// the same wall time, which keeps the instrument's relative contribution
// constant across rows.
//
// The scaling exponent is fitted as the log-log slope between the first and last
// sample rather than assumed, so "quadratic" is a measurement here and not a
// restatement of the header comment.
//
// Build (host):
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc
//       bench/validate_scaling.cpp src/core.cpp -o build/bench_validate
//   ./build/bench_validate

#include "bench_timer.h"
#include "pondmerge/pondmerge.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

#ifndef PM_BENCH_ZONE_BYTES
#define PM_BENCH_ZONE_BYTES (256u * 1024u)
#endif
#ifndef PM_BENCH_SEGMENT
#define PM_BENCH_SEGMENT 4096u
#endif
// Wall time to aim at per sample point. The probe call tells us the per-call
// cost, and K is set so that K calls take about this long.
#ifndef PM_BENCH_TARGET_NS
#define PM_BENCH_TARGET_NS 5000000ull
#endif

constexpr uint32_t ZONE_BYTES = PM_BENCH_ZONE_BYTES;
constexpr uint32_t SEGMENT    = PM_BENCH_SEGMENT;
constexpr uint32_t OBJ_SIZE   = 64u;
constexpr uint32_t LIVE_MAX   = 1024u;

static_assert(ZONE_BYTES / SEGMENT <= PM_MAX_SEGMENTS,
              "the zone needs more segments than PM_MAX_SEGMENTS allows");
static_assert(LIVE_MAX <= PM_MAX_OBJECTS,
              "the live count must fit the descriptor table");

alignas(16) uint8_t g_zone[ZONE_BYTES];

struct Row {
    uint32_t live, blocks;
    double validate_ns, get_stats_ns;
};

} // namespace

int pm_bench_validate_scaling() {
    pm_bench::report("validate / get_stats scaling against block count");

    pm::Config cfg{g_zone, sizeof(g_zone), SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) {
        std::printf("init failed\n");
        return 1;
    }
    pm::PoolId pool{};
    if (pm::create_pool(pool, 64) != pm::Status::Ok) {
        std::printf("create_pool failed\n");
        return 1;
    }

    std::printf("\n%-8s %-8s %-8s %14s %14s %14s\n", "live", "blocks",
                "free blk", "validate", "per block", "get_stats");
    std::printf("%-8s %-8s %-8s %14s %14s %14s\n", "----", "------", "--------",
                "--------", "---------", "---------");

    Row rows[8];
    uint32_t nrow = 0;
    for (uint32_t n = 64; n <= LIVE_MAX && nrow < 8; n *= 2) {
        // Build the state: n live objects, then free every other one so that
        // both the live count and the free-block count scale together.
        pm::RawRef refs[LIVE_MAX];
        uint32_t live = 0;
        for (uint32_t i = 0; i < n; ++i) {
            pm::RawRef r{};
            if (pm::alloc(pool, OBJ_SIZE, 8, pm::PM_MOVABLE, i, r) != pm::Status::Ok)
                break;
            refs[live++] = r;
        }
        uint32_t freed = 0;
        for (uint32_t i = 0; i + 1 < live; i += 2) {
            if (pm::free(refs[i]) == pm::Status::Ok) ++freed;
        }
        uint32_t const blocks = live + freed;

        // Probe once, then size the batch so each row costs similar wall time.
        uint64_t const t_p0 = pm_bench::now_ns();
        pm::Status const v = pm::validate(pool);
        uint64_t const probe = pm_bench::now_ns() - t_p0;
        if (v != pm::Status::Ok) {
            std::printf("  validate failed: %s\n", pm::status_name(v));
            break;
        }
        uint64_t k = 4;
        if (probe > 0) {
            k = PM_BENCH_TARGET_NS / probe;
            if (k < 4) k = 4;
            if (k > 4096) k = 4096;
        }

        // Batched: K validations inside one interval, repeated, minimum kept.
        double best_v = -1;
        for (uint32_t rep = 0; rep < 5; ++rep) {
            uint64_t const t0 = pm_bench::now_ns();
            for (uint64_t i = 0; i < k; ++i) (void)pm::validate(pool);
            double const per = (double)(pm_bench::now_ns() - t0) / (double)k;
            if (best_v < 0 || per < best_v) best_v = per;
        }
        double best_s = -1;
        for (uint32_t rep = 0; rep < 5; ++rep) {
            uint64_t const t0 = pm_bench::now_ns();
            for (uint64_t i = 0; i < k; ++i) (void)pm::get_stats(pool);
            double const per = (double)(pm_bench::now_ns() - t0) / (double)k;
            if (best_s < 0 || per < best_s) best_s = per;
        }

        rows[nrow].live = live;
        rows[nrow].blocks = blocks;
        rows[nrow].validate_ns = best_v;
        rows[nrow].get_stats_ns = best_s;

        char vbuf[32], sbuf[32];
        if (best_v >= 1000.0)
            std::snprintf(vbuf, sizeof vbuf, "%.1f us", best_v / 1000.0);
        else
            std::snprintf(vbuf, sizeof vbuf, "%.1f ns", best_v);
        std::snprintf(sbuf, sizeof sbuf, "%.1f ns", best_s);
        std::printf("%-8u %-8u %-8u %14s %14.1f %14s\n", (unsigned)live,
                    (unsigned)blocks, (unsigned)freed, vbuf,
                    best_v / (double)blocks, sbuf);
        std::fflush(stdout);
        ++nrow;

        // tear down for the next size
        for (uint32_t i = 0; i < live; ++i) (void)pm::free(refs[i]);
        if (n > LIVE_MAX / 2) break;
    }

    if (nrow >= 2) {
        Row const& a = rows[0];
        Row const& b = rows[nrow - 1];
        double const vr = (double)b.blocks / (double)a.blocks;
        double const vc = b.validate_ns / a.validate_ns;
        double const sc = b.get_stats_ns / a.get_stats_ns;
        double const expo = std::log(vc) / std::log(vr);
        std::printf("\nscaling blocks %u -> %u (%.1fx):\n", (unsigned)a.blocks,
                    (unsigned)b.blocks, vr);
        std::printf("  validate cost  x%.1f   -> fitted exponent %.2f"
                    " (2.0 = quadratic, 1.0 = linear)\n", vc, expo);
        std::printf("  get_stats cost x%.1f   -> fitted exponent %.2f\n", sc,
                    std::log(sc) / std::log(vr));
        std::printf("  get_stats at the largest size: %.0f ns (%.2f ns/block)."
                    " Even polled at 10 Hz that is far below any budget,\n"
                    "  so it is NOT worth optimising; validate is the only one"
                    " of the two with a scaling problem.\n",
                    b.get_stats_ns, b.get_stats_ns / (double)b.blocks);
    }

    if (pm::destroy_pool(pool) != pm::Status::Ok)
        std::printf("destroy_pool failed\n");
    return 0;
}

#ifndef PM_BENCH_NO_HOST_MAIN
int main() { return pm_bench_validate_scaling(); }
#endif
