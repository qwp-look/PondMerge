// PondMerge v1 acceptance suite on ESP32-S3 (n16r8).
// Runs the same test groups as the host runner over a 256 KiB Auto Zone in
// internal SRAM, then the dual-core lock-boundary test, then reports
// pass/fail. The suites live in tests/suite.cpp and tests/concurrency_esp32.cpp.
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdint>

extern "C" void app_main(void);
int pondmerge_run_tests(uint32_t stress_ops);
int pondmerge_run_concurrency_tests(void);

#ifndef PM_DEVICE_STRESS_OPS
#define PM_DEVICE_STRESS_OPS 2000
#endif

extern "C" void app_main(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    printf("\n=== PondMerge v1 ESP32-S3 acceptance suite ===\n");
    printf("chip: model=%d rev=%d.%d cores=%d\n", (int)info.model,
           info.revision >> 4, info.revision & 0xf, (int)info.cores);
    printf("stress ops: %d (override with -DPM_DEVICE_STRESS_OPS=...)\n",
           (int)PM_DEVICE_STRESS_OPS);
    printf("free internal heap at boot: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fflush(stdout);

    int rc = pondmerge_run_tests(PM_DEVICE_STRESS_OPS);
    printf("=== suite %s (rc=%d) ===\n", rc == 0 ? "PASSED" : "FAILED", rc);
    fflush(stdout);

    int rc2 = pondmerge_run_concurrency_tests();
    printf("=== concurrency %s (rc=%d) ===\n", rc2 == 0 ? "PASSED" : "FAILED", rc2);
    fflush(stdout);
}
