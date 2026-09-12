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
