#!/bin/sh
# PondMerge host gate runner: the twelve checks from CONTRIBUTING.md section 3,
# one command. Everything here is a straight call into tests/run_host.sh (or
# the demo smokes), so this file documents the gates rather than replacing
# them: each line is the same command a human would run by hand, and each
# gate's own output is what you read when it fails.
#
#   scripts/gates.sh            # all twelve gates, in CONTRIBUTING.md's order
#   scripts/gates.sh -v         # same, without collapsing each gate's output
#
# The suite/model gates are COUNTED, not just pass/fail: run_gate_checks
# parses the "N checks" summary lines and enforces a floor per gate. The
# floors are deliberately below the measured values (Debug/Release ~5.45M
# suite checks, San ~1.55M, model 465,124, seg1024 ~4.18M) so ordinary edits
# and added tests do not trip them, while a silently SHRUNK run -- the
# ops-parsed-to-zero accident that made --seg1024 a false green, or a dropped
# stress section -- fails loudly instead of passing with 0 failures.
# (The San figure is the 3000-op default of run_host.sh --san; Debug and
# Release run 10000 ops.)
#
# The demo binary is REBUILT on every gates run: the smokes exercise whatever
# binary is on disk, so a stale build/host_demo would silently gate old code.
#
# This script does NOT cover the device targets: flashing and capturing a real
# board is documented in CONTRIBUTING.md section 4 and is deliberately not
# automatable from a fresh clone (it needs the hardware).

set -u
cd "$(dirname "$0")/.."

verbose=0
[ "${1:-}" = "-v" ] && verbose=1

mkdir -p build
# The demo smokes need the demo binary; CI builds it with -Werror.
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/host_demo.cpp src/core.cpp -o build/host_demo || exit 1

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

# Like run_gate, plus a floor on the run's reported work. Parses the last two
# "N checks" summary lines (suite total, then model total) and fails the gate
# when either is below its floor or missing. $2 = suite floor, $3 = model floor.
run_gate_checks() {
    name=$1; suite_floor=$2; model_floor=$3
    shift 3
    printf '=== %s\n' "$name"
    out=$("$@" 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        counts=$(printf '%s\n' "$out" | grep -Eo '[0-9]+ checks' | grep -Eo '[0-9]+')
        suite_n=$(printf '%s\n' "$counts" | sed -n 1p)
        model_n=$(printf '%s\n' "$counts" | sed -n 2p)
        if [ -z "$suite_n" ] || [ -z "$model_n" ]; then
            fail=$((fail + 1))
            printf '  no check counts found in output -- refused to count this as a pass\n'
            printf '%s\n' "$out" | tail -10 | sed 's/^/  /'
            printf '^^^ FAILED: %s\n' "$name"
            return
        fi
        ok=1
        [ "$suite_n" -ge "$suite_floor" ] || { printf '  suite checks %s < floor %s\n' "$suite_n" "$suite_floor"; ok=0; }
        [ "$model_n" -ge "$model_floor" ] || { printf '  model checks %s < floor %s\n' "$model_n" "$model_floor"; ok=0; }
        if [ $ok -eq 1 ]; then
            pass=$((pass + 1))
            printf '  %s suite checks, %s model checks (floors %s/%s)\n' "$suite_n" "$model_n" "$suite_floor" "$model_floor"
        else
            fail=$((fail + 1))
            printf '%s\n' "$out" | tail -10 | sed 's/^/  /'
            printf '^^^ FAILED: %s\n' "$name"
        fi
    else
        fail=$((fail + 1))
        printf '%s\n' "$out" | tail -30 | sed 's/^/  /'
        printf '^^^ FAILED: %s\n' "$name"
    fi
}

run_gate_checks "1/12 suite + model (Debug, 10000 ops)"   5000000 400000 tests/run_host.sh
run_gate_checks "2/12 suite + model (Release)"            5000000 400000 tests/run_host.sh --release
run_gate_checks "3/12 suite + model (ASan+UBSan)"        1400000 400000 tests/run_host.sh --san
run_gate "4/12 cppcheck (2.19.0-calibrated)"     tests/run_host.sh --cppcheck
run_gate "5/12 TLSF configuration matrix"        tests/run_host.sh --configs
run_gate "6/12 coverage (Debug + Release, floors 85%/75%)" tests/run_host.sh --coverage
run_gate "7/12 libFuzzer (bounded)"              tests/run_host.sh --fuzz
run_gate "8/12 demo protocol regression"         python3 examples/protocol_smoke.py build/host_demo
run_gate "9/12 demo HTTP regression"             python3 examples/http_smoke.py build/host_demo
run_gate "10/12 sensor-pipeline example (self-audited)" sh -c '
    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
        examples/sensor_pipeline.cpp src/core.cpp -o build/sensor_pipeline || exit 1
    ./build/sensor_pipeline > build/sensor_pipeline.log || exit 1
    grep -q "=== sensor pipeline done: ALL AUDITS PASSED ===" build/sensor_pipeline.log || {
        echo "example audits did not all pass"; exit 1
    }
    grep -qE "compactions: [1-9]" build/sensor_pipeline.log || {
        echo "compaction flow did not fire at this sizing"; exit 1
    }'
run_gate "11/12 benchmarks build (Release -Werror)" sh -c '
    F="-std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Wall -Wextra -Wno-unused-parameter -Werror -Iinclude -Isrc"
    status=0
    for b in alloc_latency validate_scaling fragmentation compaction_window host_insn churn_overhead; do
        g++ $F bench/$b.cpp src/core.cpp -o build/bench_gate_$b || status=1
    done
    exit $status'
run_gate_checks "12/12 suite + model (seg1024 geometry, Release)" 4000000 400000 \
    tests/run_host.sh --seg1024-release

printf '\n=== gates: %d passed, %d failed ===\n' "$pass" "$fail"
[ "$fail" = 0 ]
