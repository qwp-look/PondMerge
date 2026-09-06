#!/bin/sh
# TLSF configuration matrix (task-book v2 section 7 / 11.1.6-7).
# Every declared PM_SL_COUNT in [2,16] must build and pass the smoke test;
# PM_SL_COUNT=32 must FAIL TO COMPILE (uint16 bitmap would truncate).
set -e
cd "$(dirname "$0")/.."
for sl in 2 4 8 16; do
    g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 \
        -DPM_SL_COUNT=$sl -Iinclude -Isrc \
        src/core.cpp tests/config_smoke.cpp -o build/config_smoke_$sl
    ./build/config_smoke_$sl
done
# FL_MAX at its ceiling must work too.
g++ -std=c++17 -O1 -DPM_FL_MAX=31 -Iinclude -Isrc \
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
echo "configuration matrix PASSED"
