// host_insn.cpp -- instruction count of the churn pair on the HOST, for
// comparison with the device's own count from the Xtensa performance counters.
//
// WHY
// bench/RESULTS.md section 6.3 records that the device costs ~17x more *cycles*
// per free+alloc than the host (and ~265x more time, of which the clock accounts
// for 15.4x). perfcount.cpp then showed on the target that the cost is NOT cache
// misses -- both miss counters read exactly zero -- but instruction count:
// 1,528 instructions per pair at CPI 1.41. That leaves an obvious question the
// target cannot answer on its own: how many instructions does the SAME code
// execute on x86-64?
//
// The host's perf events are blocked in this VM (perf_event_paranoid), but
// callgrind counts instructions deterministically, so this program is written to
// be run under it. Nothing here is timed: the deliverable is an instruction
// count, and it is deliberately the same workload, configuration and seed as
// churn_overhead.cpp's row A.
//
// Build:
//   g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -DPM_MAX_OBJECTS=256 \
//       -Iinclude -Isrc bench/host_insn.cpp src/core.cpp -o /tmp/host_insn
//   for n in 4096 8192; do
//     valgrind --tool=callgrind --callgrind-out-file=/tmp/cg_$n.out
//       /tmp/host_insn $n >/dev/null 2>&1
//     grep -m1 '^summary:' /tmp/cg_$n.out
//   done
//
// The count is taken as a DIFFERENTIAL between two run lengths, so the process's
// own startup and teardown cancel and what remains is the churn itself. (Same
// trick as the library's own `free`/`alloc` attribution, for the same reason.)

#include "pondmerge/pondmerge.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// Identical to churn_overhead.cpp's defaults, so the two instruction counts
// describe the same work.
constexpr uint32_t ZONE_BYTES = 256u * 1024u;
constexpr uint32_t SEGMENT    = 4096u;
constexpr uint32_t SEGMENTS   = 64u;
constexpr uint32_t LIVE       = 256u;
constexpr uint32_t RING       = 1024u;
constexpr uint64_t SEED       = 0x9E3779B97F4A7C15ull;

const uint32_t kSizes[] = {32, 48, 64, 96, 128, 192, 256};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

alignas(16) uint8_t g_zone[ZONE_BYTES];

struct Slot { pm::RawRef ref; uint32_t size; };

Slot     g_slots[LIVE];
uint32_t g_idx[RING];
uint64_t g_rng = SEED;
uint32_t g_sink = 0;
pm::PoolId g_pool{};

inline uint32_t rng() {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g_rng >> 33);
}

inline void churn_at(uint32_t i) {
    uint32_t const sz = g_slots[i].size;
    (void)pm::free(g_slots[i].ref);
    pm::RawRef r{};
    (void)pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r);
    g_slots[i].ref = r;
}

} // namespace

int main(int argc, char** argv) {
    uint32_t const ops = (argc > 1) ? (uint32_t)std::strtoul(argv[1], nullptr, 10)
                                    : 4096u;

    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    if (pm::init(cfg) != pm::Status::Ok) return 1;
    if (pm::create_pool(g_pool, SEGMENTS) != pm::Status::Ok) return 1;

    for (uint32_t i = 0; i < LIVE; ++i) {
        uint32_t const sz = kSizes[i % kSizeCount];
        pm::RawRef r{};
        if (pm::alloc(g_pool, sz, 8, pm::PM_MOVABLE, i, r) != pm::Status::Ok)
            return 1;
        g_slots[i].ref = r;
        g_slots[i].size = sz;
    }
    g_rng = SEED;
    for (uint32_t n = 0; n < RING; ++n) g_idx[n] = rng() % LIVE;

    for (uint32_t n = 0; n < ops; ++n) churn_at(g_idx[n & (RING - 1)]);

    // A checksum printed so the loop cannot be dead-code-eliminated, and so a
    // reader can confirm the two run lengths did the same kind of work.
    g_sink = 0;
    for (uint32_t i = 0; i < LIVE; ++i) g_sink += g_slots[i].ref.index;
    std::printf("ops=%u sink=%u\n", (unsigned)ops, (unsigned)g_sink);

    for (uint32_t i = 0; i < LIVE; ++i) (void)pm::free(g_slots[i].ref);
    (void)pm::deinit();
    return 0;
}
