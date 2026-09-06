// Host runner for the PondMerge acceptance suite.
// The suite itself lives in tests/suite.cpp (shared with the ESP32-S3 app).
#include <cstdio>
#include <cstdint>
#include <cstdlib>

int pondmerge_run_tests(uint32_t stress_ops);

int main(int argc, char** argv) {
    uint32_t ops = argc > 1 ? (uint32_t)atoi(argv[1]) : 10000;
    return pondmerge_run_tests(ops);
}
