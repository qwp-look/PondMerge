#!/bin/sh
# TLSF configuration matrix (task-book v2 section 7 / 11.1.6-7).
#   * every declared PM_SL_COUNT in [2,16] must build and pass the smoke test;
#   * PM_SL_COUNT=32 must FAIL TO COMPILE (uint16 bitmap would truncate);
#   * PM_FL_MAX=31 must build and pass; PM_FL_MAX=32 must fail to compile;
#   * a zone at the 2^PM_FL_MAX ceiling must be refused at init, and a zone
#     below it must be accepted and usable.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
for sl in 2 4 8 16; do
    g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 \
        -DPM_SL_COUNT=$sl -Iinclude -Isrc \
        src/core.cpp tests/config_smoke.cpp -o build/config_smoke_$sl
    ./build/config_smoke_$sl
done
# FL_MAX at its ceiling must work too.
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 -DPM_FL_MAX=31 -Iinclude -Isrc \
    src/core.cpp tests/config_smoke.cpp -o build/config_smoke_fl31
./build/config_smoke_fl31
# Out-of-contract configurations must be rejected at compile time.
if g++ -std=c++17 -DPM_SL_COUNT=32 -Iinclude -Isrc -fsyntax-only src/core.cpp tests/config_smoke.cpp 2>/dev/null; then
    echo "FAIL: PM_SL_COUNT=32 compiled but must be rejected"; exit 1
fi
echo "PM_SL_COUNT=32 correctly rejected at compile time"
if g++ -std=c++17 -DPM_FL_MAX=32 -Iinclude -Isrc -fsyntax-only src/core.cpp tests/config_smoke.cpp 2>/dev/null; then
    echo "FAIL: PM_FL_MAX=32 compiled but must be rejected"; exit 1
fi
echo "PM_FL_MAX=32 correctly rejected at compile time"
# FL capacity contract: the zone ceiling is refused, just below it is accepted.
for fl in 16 24; do
    g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 -DPM_FL_MAX=$fl -Iinclude -Isrc \
        src/core.cpp tests/config_limits.cpp -o build/config_limits_$fl
    ./build/config_limits_$fl refuse
    ./build/config_limits_$fl accept
done
# PM_MIN_BLOCK=32 (A4-08): the minimum block grows to 32 B; alloc(1) and
# alloc(8) must both occupy a 32-byte block and the pool must stay valid.
# The variant carries its own assertions, so it is generated here rather than
# reusing config_smoke.cpp (which asserts nothing about block sizes).
cat > build/config_minblock32.cpp <<'EOF'
#include "pondmerge/pondmerge.hpp"
#include "../src/internal.h"
#include <cstdio>

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    if (pm::init({zone, sizeof(zone), 4096}) != pm::Status::Ok) return 1;
    pm::PoolId pool{};
    if (pm::create_pool(pool, 8) != pm::Status::Ok) return 1;
    pm::RawRef r1{}, r8{};
    if (pm::alloc(pool, 1, 8, 0, 1, r1) != pm::Status::Ok) return 1;
    if (pm::alloc(pool, 8, 8, 0, 2, r8) != pm::Status::Ok) return 1;
    using namespace pm::internal;
    if (g().objects[r1.index].block_size != 32) return 1; // PM_MIN_BLOCK, not 16
    if (g().objects[r8.index].block_size != 32) return 1;
    if (pm::get_stats(pool).used_bytes != 64) return 1;
    if (pm::validate(pool) != pm::Status::Ok) return 1;
    if (pm::free(r1) != pm::Status::Ok || pm::free(r8) != pm::Status::Ok) return 1;
    if (pm::deinit() != pm::Status::Ok) return 1;
    printf("config smoke OK (PM_MIN_BLOCK=32)\n");
    return 0;
}
EOF
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 -DPM_MIN_BLOCK=32 -Iinclude -Isrc \
    src/core.cpp build/config_minblock32.cpp -o build/config_minblock32
./build/config_minblock32
echo "configuration matrix PASSED"
