# Consumer smoke test for the plain-CMake (non-IDF) consumption path.
#
# Proves that the install + find_package(pondmerge) route actually works end to
# end: configure the library, build it, install it to a scratch prefix, then
# build a SEPARATE tiny project against the installed package and run it.
# CI runs this; run it locally with:  tests/consumer_smoke.sh
set -e

cd "$(dirname "$0")/.."
ROOT="$PWD"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PREFIX="$WORK/prefix"
CONSUMER="$WORK/consumer"

echo "== 1/4 configure + build + install the library =="
cmake -S "$ROOT" -B "$WORK/lib-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$WORK/lib-cfg.log" 2>&1 || {
        echo "FAIL: library configure"; cat "$WORK/lib-cfg.log"; exit 1; }
cmake --build "$WORK/lib-build" -j > "$WORK/lib-build.log" 2>&1 || {
    echo "FAIL: library build"; cat "$WORK/lib-build.log"; exit 1; }
cmake --install "$WORK/lib-build" > "$WORK/lib-install.log" 2>&1 || {
    echo "FAIL: library install"; cat "$WORK/lib-install.log"; exit 1; }
echo "   installed to $PREFIX"

echo "== 2/4 generate a separate consumer project =="
mkdir -p "$CONSUMER"
cat > "$CONSUMER/CMakeLists.txt" <<'CMAKE_EOF'
cmake_minimum_required(VERSION 3.16)
project(consumer CXX)
find_package(pondmerge 1.0 REQUIRED)
add_executable(smoke main.cpp)
target_link_libraries(smoke PRIVATE pondmerge::pondmerge)
CMAKE_EOF

cat > "$CONSUMER/main.cpp" <<'CPP_EOF'
// Exercises the public API only -- exactly what a third-party embedder sees.
#include "pondmerge/pondmerge.hpp"

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    pm::Config cfg{zone, sizeof(zone), 4096};
    if (pm::init(cfg) != pm::Status::Ok) return 1;
    pm::PoolId pool{};
    if (pm::create_pool(pool, 16) != pm::Status::Ok) return 2;

    pm::RawRef ref{};
    if (pm::alloc(pool, 128, 8, pm::PM_MOVABLE, 7, ref) != pm::Status::Ok) return 3;
    void* addr = nullptr;
    if (pm::borrow_begin(ref, 128, 8, addr) != pm::Status::Ok) return 4;
    pm::borrow_end(ref);
    if (pm::compact(pool) != pm::Status::Ok) return 5;
    if (pm::validate(pool) != pm::Status::Ok) return 6;
    if (pm::free(ref) != pm::Status::Ok) return 7;
    pm::GlobalStats gs = pm::global_stats();
    if (gs.metadata_bytes == 0) return 8;
    if (pm::deinit() != pm::Status::Ok) return 9;
    return 0;
}
CPP_EOF

echo "== 3/4 configure + build the consumer against the installed package =="
cmake -S "$CONSUMER" -B "$CONSUMER/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/con-cfg.log" 2>&1 || {
        echo "FAIL: consumer configure (find_package did not resolve)"; cat "$WORK/con-cfg.log"; exit 1; }
cmake --build "$CONSUMER/build" -j > "$WORK/con-build.log" 2>&1 || {
    echo "FAIL: consumer build"; cat "$WORK/con-build.log"; exit 1; }

echo "== 4/4 run the consumer =="
"$CONSUMER/build/smoke"
echo "consumer smoke PASSED (exit 0)"

# ---------------------------------------------------------------------------
# Path 2 of the README's integration section: add_subdirectory(). This path
# has its own trap: a consumer's plain set(PM_MAX_OBJECTS 128) must actually
# win over the library's CACHE default (CMP0126, needs 3.21+). Before the
# policy bump the override was silently ignored and the library compiled with
# 1024 objects (~116 KiB of metadata) -- exactly what the README warns small
# targets about, with no diagnostic. The consumer asserts the small-table
# metadata budget instead, so a regression here fails the smoke.
# ---------------------------------------------------------------------------
echo "== 5/5 add_subdirectory path: set(PM_MAX_OBJECTS) must take effect =="
SUB="$WORK/subconsumer"
mkdir -p "$SUB"
cat > "$SUB/CMakeLists.txt" <<'CMAKE_EOF'
cmake_minimum_required(VERSION 3.16)
project(subconsumer CXX)
set(PM_MAX_OBJECTS 128)                 # the documented IDF-style override
set(PM_MAX_POOLS 4)                     # matches README's 128/4 budget row
add_subdirectory(${POND_MERGE_ROOT} pondmerge-ext)
add_executable(smoke2 main.cpp)
target_link_libraries(smoke2 PRIVATE pondmerge::pondmerge)
CMAKE_EOF

# 72 + 476*4 + 106*128 + 4 = 15,548 B for the 128/4 table; anything above
# ~20 KiB means the override did not reach the compile line.
cat > "$SUB/main.cpp" <<'CPP_EOF'
#include "pondmerge/pondmerge.hpp"

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    pm::Config cfg{zone, sizeof(zone), 4096};
    if (pm::init(cfg) != pm::Status::Ok) return 1;
    pm::GlobalStats gs = pm::global_stats();
    // 128 objects / 4 pools: ~15.5 KiB. The 1024/16 default is ~116 KiB.
    if (gs.metadata_bytes < 12000 || gs.metadata_bytes > 20000) return 11;
    if (pm::deinit() != pm::Status::Ok) return 2;
    return 0;
}
CPP_EOF

cmake -S "$SUB" -B "$SUB/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPOND_MERGE_ROOT="$ROOT" > "$WORK/sub-cfg.log" 2>&1 || {
        echo "FAIL: add_subdirectory consumer configure"; cat "$WORK/sub-cfg.log"; exit 1; }
cmake --build "$SUB/build" -j > "$WORK/sub-build.log" 2>&1 || {
    echo "FAIL: add_subdirectory consumer build"; cat "$WORK/sub-build.log"; exit 1; }
"$SUB/build/smoke2" || {
    echo "FAIL: PM_MAX_OBJECTS override did not take effect (metadata_bytes out of range)"
    exit 1; }
echo "add_subdirectory override PASSED (metadata within the 128-object budget)"
