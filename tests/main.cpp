// Host runner for the PondMerge acceptance suite.
// The suite itself lives in tests/suite.cpp (shared with the ESP32-S3 app);
// the reference-model differential test lives in tests/model.cpp.
#include <cstdio>
#include <cstdint>
#include <cstdlib>

int pondmerge_run_tests(uint32_t stress_ops);
uint32_t pondmerge_run_model(uint32_t zone_bytes, uint32_t ops);

int main(int argc, char** argv) {
    uint32_t ops = argc > 1 ? (uint32_t)atoi(argv[1]) : 10000;
    int rc = pondmerge_run_tests(ops);

    // The model test has its own fixed seed and its own check counter; scale
    // its op count with the caller's stress level but keep it bounded so a
    // sanitizer run stays quick.
    uint32_t model_ops = ops < 2000u ? ops * 2u : 4000u;
    printf("\n[TEST] model_differential\n");
    uint32_t failures = pondmerge_run_model(64u * 1024u, model_ops);
    printf("%s\n", failures == 0 ? "  PASS" : "  FAIL");
    if (failures != 0) rc = 1;
    return rc;
}
