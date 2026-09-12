// PondMerge validate() / get_stats() cost scaling.
// Declared bounds: validate O((live + free)^2), get_stats O(free blocks).
// Measures the constants so the decision to rewrite them is evidence-based.
#include "pondmerge/pondmerge.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>

namespace {
constexpr uint32_t ZONE_BYTES = 256u * 1024u;
constexpr uint32_t SEGMENT    = 4096u;
alignas(16) uint8_t g_zone[ZONE_BYTES];

const uint32_t kSizes[] = {32, 48, 64, 96, 128, 192, 256};
constexpr uint32_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

int main() {
    static pm::RawRef refs[2048];
    pm::Config cfg{g_zone, ZONE_BYTES, SEGMENT};
    printf("PM_MAX_OBJECTS=%d SL=%d\n", (int)PM_MAX_OBJECTS, (int)PM_SL_COUNT);
    printf("%-9s %-9s | %-16s %-14s\n", "live", "freed", "validate", "get_stats");

    const uint32_t lives[] = {64, 128, 256, 512, 1024};
    for (uint32_t li = 0; li < 5; ++li) {
        uint32_t want = lives[li];
        if (pm::init(cfg) != pm::Status::Ok) return 1;
        pm::PoolId pool{};
        if (pm::create_pool(pool, ZONE_BYTES / SEGMENT) != pm::Status::Ok) return 1;

        uint32_t made = 0;
        for (uint32_t i = 0; i < want; ++i) {
            pm::RawRef r{};
            if (pm::alloc(pool, kSizes[i % kSizeCount], 8, pm::PM_MOVABLE, i, r)
                != pm::Status::Ok)
                break;
            refs[made++] = r;
        }

        // Fragment: free every second object -> ~half as many free blocks.
        uint32_t freed = 0;
        for (uint32_t i = 0; i < made; i += 2) {
            if (pm::free(refs[i]) == pm::Status::Ok) freed++;
        }

        uint64_t t0 = now_ns();
        pm::Status v = pm::validate(pool);
        uint64_t dt = now_ns() - t0;

        uint64_t acc = 0;
        uint32_t reps = 0;
        for (; reps < 200000; ++reps) {
            uint64_t a = now_ns();
            pm::get_stats(pool);
            acc += now_ns() - a;
            if (acc > 60000000ull) break; // ~60 ms budget
        }
        printf("%-9u %-9u | %10llu ns %s |%8.1f ns\n", (unsigned)made,
               (unsigned)freed, (unsigned long long)dt,
               v == pm::Status::Ok ? "OK " : "BAD", (double)acc / (double)reps);
        // Drain the survivors so deinit() (which requires zero live objects)
        // can reset the global state for the next case.
        for (uint32_t i = 1; i < made; i += 2) (void)pm::free(refs[i]);
        if (pm::deinit() != pm::Status::Ok) {
            printf("deinit failed -- stopping\n");
            return 1;
        }
    }
    return 0;
}
