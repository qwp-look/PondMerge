// PondMerge v1 - advice owner-violation probe (round-8 guide section 3.2).
//
// Diagnoses the Debug-only advice owner gate: the main context binds the
// advice family, then a SECOND thread calls analyze_compaction -- a
// single-owner contract violation. Debug builds abort with a PondMerge
// ASSERT; Release builds perform no runtime check and complete normally
// (the contract itself still forbids this usage).
//
//   Debug build   (default -DPM_DEBUG=1):  expects abnormal termination
//   Release build (-DPM_DEBUG=0):          expects exit code 0
#include "pondmerge/pondmerge.hpp"

#include <cstdio>
#include <thread>

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    pm::Config cfg{zone, sizeof(zone), 4096};
    if (pm::init(cfg) != pm::Status::Ok) return 2;
    pm::PoolId pool{};
    if (pm::create_pool(pool, 2) != pm::Status::Ok) return 2;

    (void)pm::analyze_compaction(pool); // binds this (main) context

    std::thread violator([&] {
        // contract violation: advice from a non-owner context
        (void)pm::analyze_compaction(pool);
    });
    violator.join();

    printf("no owner violation diagnosed (Release: runtime checks are off)\n");
    pm::destroy_pool(pool);
    pm::deinit();
    return 0;
}
