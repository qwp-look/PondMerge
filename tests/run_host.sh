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
#   tests/run_host.sh --fuzz-debug [secs]  same, PM_DEBUG=1: the Debug assert
#                                     layer is itself under test (the fuzz
#                                     target only issues valid calls, so an
#                                     abort there is a real bug)
#   tests/run_host.sh --seg1024 [ops] the fixture's 1 KiB geometry on host
#                                     (PM_TEST_SEG_BYTES=1024, Debug build)
#   tests/run_host.sh --seg1024-release [ops]  ... in Release
# Every mode except --cppcheck/--configs/--coverage/--fuzz* runs tests/suite.cpp
# AND the reference model in tests/model.cpp.
#
# The compiler is ${CXX:-g++} everywhere except the fuzz modes, which need
# clang's libFuzzer and take ${FUZZ_CXX:-clang++} so an exported CXX=g++ from
# the environment cannot silently break them.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc"
case "$1" in
--release)
    "$CXX" $CXXFLAGS -O3 -DNDEBUG -DPM_DEBUG=0 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests_release
    exec ./build/pondmerge_tests_release "${2:-10000}"
    ;;
--san)
    "$CXX" $CXXFLAGS -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -static-libasan \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests_san
    exec ./build/pondmerge_tests_san "${2:-3000}"
    ;;
--seg1024|--seg1024-release)
    # The non-default fixture geometry, exercised on host exactly as the device
    # runs it (esp32/main builds PM_TEST_SEG_BYTES=1024). suite.cpp's own
    # comment points here; the arm exists since v1.2 -- before that the flag
    # silently fell through to the ops argument and ran the DEFAULT geometry.
    seg_flags="-DPM_TEST_SEG_BYTES=1024u"
    if [ "$1" = "--seg1024-release" ]; then
        "$CXX" $CXXFLAGS $seg_flags -O3 -DNDEBUG -DPM_DEBUG=0 \
            src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
            -o build/pondmerge_tests_seg1024_release
        exec ./build/pondmerge_tests_seg1024_release "${2:-10000}"
    fi
    "$CXX" $CXXFLAGS $seg_flags -g -O1 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests_seg1024
    exec ./build/pondmerge_tests_seg1024 "${2:-10000}"
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
    # rather than llvm-cov because the project builds with $CXX (g++ by default). Each pass gets
    # a private object directory (build/cov, build/cov_rel) so the .gcno/.gcda
    # files neither litter build/ nor collide between passes.
    #
    # A FLOOR is enforced on BOTH passes: see COVERAGE_MIN_LINES below. It
    # exists so that a change which silently stops exercising a branch cannot
    # pass unnoticed; it is deliberately below the measured value rather than
    # equal to it, so ordinary edits do not trip it.
    COVERAGE_MIN_LINES=${COVERAGE_MIN_LINES:-85}
    # The branch floor (branches taken at least once) is gated like the line
    # floor. It used to be printed only, which meant a branch silently rotting
    # to 0% would pass the gate.
    COVERAGE_MIN_BRANCH=${COVERAGE_MIN_BRANCH:-75}
    coverage_pass() {
        pm_debug=$1; covdir=$2; label=$3; ops=$4
        rm -rf "build/$covdir"
        mkdir -p "build/$covdir"
        # gcov resolves the source paths recorded in the .gcno relative to its
        # working directory. Run from the pass directory those paths
        # ("src/core.cpp", ...) do not exist: the summary percentages still
        # compute, but every annotated .gcov file comes out as a header-only
        # shell and per-line attribution is impossible (v17 section 9 recorded
        # exactly that symptom). Symlinking the source roots into the pass
        # directory lets gcov find them; the annotation files then carry the
        # uncovered-line markers the ledger attributes.
        ln -sfn "$PWD/src" "build/$covdir/src"
        ln -sfn "$PWD/include" "build/$covdir/include"
        ln -sfn "$PWD/tests" "build/$covdir/tests"
        for src in src/core tests/suite tests/model tests/main; do
            # Named after the SOURCE basename on purpose: gcov locates the notes
            # file by the source file's basename, so build/cov/core.gcno is what
            # `gcov -o build/cov src/core.cpp` looks for.
            obj="build/$covdir/$(basename "$src").o"
            case "$src" in
                src/core) "$CXX" $CXXFLAGS -O0 -g --coverage -DPM_DEBUG="$pm_debug" -c "$src.cpp" -o "$obj" ;;
                *)        "$CXX" $CXXFLAGS -O0 -g -DPM_DEBUG="$pm_debug" -c "$src.cpp" -o "$obj" ;;
            esac
        done
        "$CXX" --coverage -o "build/$covdir/run" build/"$covdir"/*.o
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
        echo "core.cpp [$label]: ${pct}% lines, ${brt}% branches taken  (line floor ${COVERAGE_MIN_LINES}%, branch floor ${COVERAGE_MIN_BRANCH}%)"
        awk -v p="$pct" -v f="$COVERAGE_MIN_LINES" 'BEGIN { exit (p+0 >= f+0) ? 0 : 1 }' || {
            echo "FAIL: core.cpp line coverage below the floor ($label)"; exit 1; }
        if [ -z "$brt" ]; then
            echo "FAIL: gcov produced no branch-coverage line for core.cpp ($label)"; exit 1
        fi
        awk -v p="$brt" -v f="$COVERAGE_MIN_BRANCH" 'BEGIN { exit (p+0 >= f+0) ? 0 : 1 }' || {
            echo "FAIL: core.cpp branch coverage below the floor ($label)"; exit 1; }
    }
    coverage_pass 1   cov      "Debug, PM_DEBUG=1"   "${2:-3000}"
    coverage_pass 0   cov_rel  "Release, PM_DEBUG=0" "${2:-3000}"
    echo "coverage PASSED"
    ;;
--fuzz|--fuzz-debug)
    # libFuzzer needs clang (FUZZ_CXX, NOT CXX -- an exported CXX=g++ must not
    # silently break this mode). Corpus and artifacts live under build/ so a
    # CI cache can persist the corpus between runs and a failed run keeps its
    # crash reproducer.
    FUZZ_CXX=${FUZZ_CXX:-clang++}
    if [ "$1" = "--fuzz-debug" ]; then
        # PM_DEBUG=1 puts the library's own assert layer under fuzz: the target
        # only issues VALID calls, so a Debug abort is a real library bug.
        dbg="-DPM_DEBUG=1"
    else
        # PM_DEBUG=0 on purpose for the default mode: in Debug a caller bug
        # aborts by design and libFuzzer would report that as a crash (see
        # fuzz/fuzz_pm.cpp).
        dbg="-DPM_DEBUG=0"
    fi
    "$FUZZ_CXX" -std=c++17 -g -O1 $dbg -Iinclude -Isrc \
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        fuzz/fuzz_pm.cpp src/core.cpp -o build/fuzz_pm
    mkdir -p build/fuzz_artifacts build/fuzz_corpus
    # -use_value_profile: without it the run plateaus at ~780 features almost
    # immediately, because edge coverage does not reward reaching deeper states.
    # With it the same budget yields ~3400 features and a substantially larger
    # corpus. -max_len bounds the work per input.
    exec ./build/fuzz_pm -max_total_time="${2:-60}" -use_value_profile=1 \
        -max_len=512 -print_final_stats=1 \
        -artifact_prefix=build/fuzz_artifacts/ \
        build/fuzz_corpus
    ;;
*)
    "$CXX" $CXXFLAGS -g -O1 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests
    exec ./build/pondmerge_tests "${1:-10000}"
    ;;
esac
