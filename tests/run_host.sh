#!/bin/sh
# Build and run the host acceptance tests (doc section 18).
# Usage: tests/run_host.sh [stress_ops]
set -e
cd "$(dirname "$0")/.."
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -O1 \
    -Iinclude -Isrc \
    src/core.cpp tests/suite.cpp tests/main.cpp \
    -o build/pondmerge_tests
exec ./build/pondmerge_tests "${1:-10000}"
