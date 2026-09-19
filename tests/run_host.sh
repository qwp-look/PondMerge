#!/bin/sh
# PondMerge host test runner.
#   tests/run_host.sh [ops]           Debug build (default, PM_DEBUG=1, -O1 -g)
#   tests/run_host.sh --release [ops] Release build (-O3 -DNDEBUG -DPM_DEBUG=0)
#   tests/run_host.sh --san [ops]     ASan+UBSan build (-O1, 3000 ops by default)
#   tests/run_host.sh --cppcheck      static analysis of the core + test sources
#   tests/run_host.sh --configs       TLSF configuration matrix (config_matrix.sh)
#   tests/run_host.sh --coverage      line/branch coverage of the core under
#                                     the suite, Debug AND Release passes
#   tests/run_host.sh --fuzz [secs]   build and run the libFuzzer target
# Every mode except --cppcheck/--configs/--coverage/--fuzz runs tests/suite.cpp
# AND the reference model in tests/model.cpp.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CXXFLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc"
case "$1" in
--release)
    g++ $CXXFLAGS -O3 -DNDEBUG -DPM_DEBUG=0 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests_release
    exec ./build/pondmerge_tests_release "${2:-10000}"
    ;;
--san)
    g++ $CXXFLAGS -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -static-libasan \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests_san
    exec ./build/pondmerge_tests_san "${2:-3000}"
    ;;
--cppcheck)
    # Print the version first: cppcheck's check set has changed across releases
    # (e.g. incorrectStringBooleanError and unassignedVariable fire on older
    # builds but not on 2.19.0), so a clean run proves different things on
    # different toolchains. Comparing the version is part of reading the result.
    cppcheck --version
    exec cppcheck --enable=warning,style,performance --inline-suppr \
        --error-exitcode=2 -Iinclude -Isrc --suppress=missingIncludeSystem \
        src/core.cpp tests/suite.cpp tests/model.cpp
    ;;
--configs)
    exec tests/config_matrix.sh
    ;;
--coverage)
    # Line and branch coverage of the core, measured over the acceptance suite
    # (suite + reference model), in TWO passes: Debug (PM_DEBUG=1) and Release
    # (PM_DEBUG=0). The second pass exists because R25's negative paths are
    # compiled only under Release -- a Debug-only coverage gate structurally
    # cannot see them, so "the suite covers it" would be false comfort. gcov
    # rather than llvm-cov because the project builds with g++. Each pass gets
    # a private object directory (build/cov, build/cov_rel) so the .gcno/.gcda
    # files neither litter build/ nor collide between passes.
    #
    # A FLOOR is enforced on BOTH passes: see COVERAGE_MIN_LINES below. It
    # exists so that a change which silently stops exercising a branch cannot
    # pass unnoticed; it is deliberately below the measured value rather than
    # equal to it, so ordinary edits do not trip it.
    COVERAGE_MIN_LINES=${COVERAGE_MIN_LINES:-85}
    coverage_pass() {
        pm_debug=$1; covdir=$2; label=$3; ops=$4
        rm -rf "build/$covdir"
        mkdir -p "build/$covdir"
        for src in src/core tests/suite tests/model tests/main; do
            # Named after the SOURCE basename on purpose: gcov locates the notes
            # file by the source file's basename, so build/cov/core.gcno is what
            # `gcov -o build/cov src/core.cpp` looks for.
            obj="build/$covdir/$(basename "$src").o"
            case "$src" in
                src/core) g++ $CXXFLAGS -O0 -g --coverage -DPM_DEBUG="$pm_debug" -c "$src.cpp" -o "$obj" ;;
                *)        g++ $CXXFLAGS -O0 -g -DPM_DEBUG="$pm_debug" -c "$src.cpp" -o "$obj" ;;
            esac
        done
        g++ --coverage -o "build/$covdir/run" build/"$covdir"/*.o
        ./build/"$covdir"/run "$ops" > "build/$covdir"/run.log 2>&1
        tail -4 "build/$covdir"/run.log
        # Run gcov from INSIDE the pass directory: it writes one .gcov file per
        # source into its working directory, so invoking it from the repository
        # root would leave untracked files there -- and running both passes in
        # one directory would overwrite the first pass's files.
        ( cd "build/$covdir" && gcov -b -c -o . ../../src/core.cpp > gcov.txt 2>&1 ) || true
        # Read core.cpp's OWN block. gcov prints one block per contributing
        # file and happens to emit the named file first, but taking "the first
        # Lines executed line" would silently report a header's coverage if
        # that ordering ever changed, so the block is selected by its File
        # header.
        awk '/^File .*core\.cpp/{f=1} /^File /{if ($0 !~ /core\.cpp/) f=0}
             f && /^(Lines executed|Branches executed|Taken at least once|Calls executed)/{print}
            ' "build/$covdir/gcov.txt" || true
        pct=$(awk '/^File .*core\.cpp/{f=1; next} /^File /{f=0}
                    f && /^Lines executed/ {sub(/.*:/,""); sub(/%.*/,""); print; exit}
                   ' "build/$covdir/gcov.txt")
        brt=$(awk '/^File .*core\.cpp/{f=1; next} /^File /{f=0}
                    f && /^Taken at least once/ {sub(/.*:/,""); sub(/%.*/,""); print; exit}
                   ' "build/$covdir/gcov.txt")
        if [ -z "$pct" ]; then
            echo "FAIL: gcov produced no line-coverage line for core.cpp ($label); raw output follows"
            cat "build/$covdir/gcov.txt"
            exit 1
        fi
        echo "core.cpp [$label]: ${pct}% lines, ${brt}% branches taken  (line floor ${COVERAGE_MIN_LINES}%)"
        awk -v p="$pct" -v f="$COVERAGE_MIN_LINES" 'BEGIN { exit (p+0 >= f+0) ? 0 : 1 }' || {
            echo "FAIL: core.cpp line coverage below the floor ($label)"; exit 1; }
    }
    coverage_pass 1   cov      "Debug, PM_DEBUG=1"   "${2:-3000}"
    coverage_pass 0   cov_rel  "Release, PM_DEBUG=0" "${2:-3000}"
    echo "coverage PASSED"
    ;;
--fuzz)
    # libFuzzer needs clang. PM_DEBUG=0 on purpose: in Debug a caller bug aborts
    # by design and libFuzzer would report that as a crash (see fuzz/fuzz_pm.cpp).
    CXX=${CXX:-clang++}
    "$CXX" -std=c++17 -g -O1 -DPM_DEBUG=0 -Iinclude -Isrc \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        fuzz/fuzz_pm.cpp src/core.cpp -o build/fuzz_pm
    mkdir -p build/fuzz_artifacts
    # -use_value_profile: without it the run plateaus at ~780 features almost
    # immediately, because edge coverage does not reward reaching deeper states.
    # With it the same budget yields ~3400 features and a substantially larger
    # corpus. -max_len bounds the work per input.
    exec ./build/fuzz_pm -max_total_time="${2:-60}" -use_value_profile=1 \
        -max_len=512 -print_final_stats=1 \
        -artifact_prefix=build/fuzz_artifacts/
    ;;
*)
    g++ $CXXFLAGS -g -O1 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests
    exec ./build/pondmerge_tests "${1:-10000}"
    ;;
esac
