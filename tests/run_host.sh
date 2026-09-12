#!/bin/sh
# PondMerge host test runner.
#   tests/run_host.sh [ops]        Debug build (default, PM_DEBUG=1, -O1 -g)
#   tests/run_host.sh --release    Release build (-O3 -DNDEBUG -DPM_DEBUG=0)
#   tests/run_host.sh --san        ASan+UBSan build (-O1, 3000 ops by default)
#   tests/run_host.sh --cppcheck   static analysis of the core + test sources
#   tests/run_host.sh --configs    TLSF configuration matrix (config_matrix.sh)
# Every mode runs tests/suite.cpp AND the reference model in tests/model.cpp.
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
*)
    g++ $CXXFLAGS -g -O1 \
        src/core.cpp tests/suite.cpp tests/model.cpp tests/main.cpp \
        -o build/pondmerge_tests
    exec ./build/pondmerge_tests "${1:-10000}"
    ;;
esac
