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
echo "configuration matrix PASSED"
