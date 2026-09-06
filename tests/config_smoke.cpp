// Minimal smoke suite compiled once per TLSF configuration by
// tests/run_host.sh --configs (task-book v2 7.1/11.1: every declared
// PM_SL_COUNT/PM_FL_MAX combination must work; rejected ones must fail to
// compile).
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h"
#include <cstdio>
#include <cstring>

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    pm::Config cfg{zone, sizeof(zone), 4096};
    if (pm::init(cfg) != pm::Status::Ok) return 1;
    pm::PoolId pool{};
    if (pm::create_pool(pool, 8) != pm::Status::Ok) return 1;
    pm::RawRef refs[32];
    for (uint32_t i = 0; i < 32; ++i)
        if (pm::alloc(pool, 64 + i * 13, 8, 0, i, refs[i]) != pm::Status::Ok) return 1;
    for (uint32_t i = 0; i < 32; i += 2)
        if (pm::free(refs[i]) != pm::Status::Ok) return 1;
    if (pm::compact(pool) != pm::Status::Ok) return 1;
    if (pm::validate(pool) != pm::Status::Ok) return 1;
    for (uint32_t i = 1; i < 32; i += 2)
        if (pm::free(refs[i]) != pm::Status::Ok) return 1;
    pm::PoolStats st = pm::get_stats(pool);
    if (st.used_bytes != 0 || st.object_count != 0) return 1;
    if (pm::deinit() != pm::Status::Ok) return 1;
    printf("config smoke OK (SL=%d FL_MAX=%d)\n", PM_SL_COUNT, PM_FL_MAX);
    return 0;
}
