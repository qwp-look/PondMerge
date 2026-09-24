#!/bin/sh
# PondMerge host gate runner: the eleven checks from CONTRIBUTING.md section 3,
# one command. Everything here is a straight call into tests/run_host.sh (or
# the demo smokes), so this file documents the gates rather than replacing
# them: each line is the same command a human would run by hand, and each
# gate's own output is what you read when it fails.
#
#   scripts/gates.sh            # all eleven gates, in CONTRIBUTING.md's order
#   scripts/gates.sh -v         # same, without collapsing each gate's output
#
# The expected pass counts (5,432,806 / 5,432,813 / 1,531,817 checks; model
# 466,859; protocol 95; HTTP 15) are stated in CONTRIBUTING.md section 3 -- a
# gate that "passes" with different numbers is itself worth investigating.
# (The San figure is the 3000-op default of run_host.sh --san; Debug and
# Release run 10000 ops.)
#
# This script does NOT cover the device targets: flashing and capturing a real
# board is documented in CONTRIBUTING.md section 4 and is deliberately not
# automatable from a fresh clone (it needs the hardware).

set -u
cd "$(dirname "$0")/.."

verbose=0
[ "${1:-}" = "-v" ] && verbose=1

mkdir -p build
if [ ! -x build/host_demo ]; then
    # The demo smokes need the demo binary; CI builds it with -Werror.
    g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
        examples/host_demo.cpp src/core.cpp -o build/host_demo || exit 1
fi

pass=0
fail=0

run_gate() {
    name=$1
    shift
    printf '=== %s\n' "$name"
    if [ "$verbose" = 1 ]; then
        if "$@"; then pass=$((pass + 1)); else fail=$((fail + 1)); printf '^^^ FAILED: %s\n' "$name"; fi
    else
        if out=$("$@" 2>&1); then
            pass=$((pass + 1))
            printf '%s\n' "$out" | tail -2 | sed 's/^/  /'
        else
            fail=$((fail + 1))
            printf '%s\n' "$out" | tail -30 | sed 's/^/  /'
            printf '^^^ FAILED: %s\n' "$name"
        fi
    fi
}

run_gate "1/11 suite + model (Debug, 10000 ops)" tests/run_host.sh
run_gate "2/11 suite + model (Release)"          tests/run_host.sh --release
run_gate "3/11 suite + model (ASan+UBSan)"       tests/run_host.sh --san
run_gate "4/11 cppcheck (2.19.0-calibrated)"     tests/run_host.sh --cppcheck
run_gate "5/11 TLSF configuration matrix"        tests/run_host.sh --configs
run_gate "6/11 coverage (Debug + Release, floor 85%)" tests/run_host.sh --coverage
run_gate "7/11 libFuzzer (bounded)"              tests/run_host.sh --fuzz
run_gate "8/11 demo protocol regression"         python3 examples/protocol_smoke.py build/host_demo
run_gate "9/11 demo HTTP regression"             python3 examples/http_smoke.py build/host_demo
run_gate "10/11 sensor-pipeline example (self-audited)" sh -c '
    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
        examples/sensor_pipeline.cpp src/core.cpp -o build/sensor_pipeline || exit 1
    ./build/sensor_pipeline > build/sensor_pipeline.log || exit 1
    grep -q "=== sensor pipeline done: ALL AUDITS PASSED ===" build/sensor_pipeline.log || {
        echo "example audits did not all pass"; exit 1
    }
    grep -qE "compactions: [1-9]" build/sensor_pipeline.log || {
        echo "compaction flow did not fire at this sizing"; exit 1
    }'
run_gate "11/11 benchmarks build (Release -Werror)" sh -c '
    F="-std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Wall -Wextra -Wno-unused-parameter -Werror -Iinclude -Isrc"
    status=0
    for b in alloc_latency validate_scaling fragmentation compaction_window; do
        g++ $F bench/$b.cpp src/core.cpp -o build/bench_gate_$b || status=1
    done
    exit $status'

printf '\n=== gates: %d passed, %d failed ===\n' "$pass" "$fail"
[ "$fail" = 0 ]
