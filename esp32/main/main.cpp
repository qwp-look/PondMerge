// PondMerge v1 acceptance suite on ESP32-S3 (n16r8).
// Runs the same test groups as the host runner over a 256 KiB Auto Zone in
// internal SRAM, then reports pass/fail. The suite lives in tests/suite.cpp.
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdint>

extern "C" void app_main(void);
int pondmerge_run_tests(uint32_t stress_ops);

extern "C" void app_main(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    printf("\n=== PondMerge v1 ESP32-S3 acceptance suite ===\n");
    printf("chip: model=%d rev=%d.%d cores=%d\n", (int)info.model,
           info.revision >> 4, info.revision & 0xf, (int)info.cores);
    printf("free internal heap at boot: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fflush(stdout);

    int rc = pondmerge_run_tests(2000);
    printf("=== suite %s (rc=%d) ===\n", rc == 0 ? "PASSED" : "FAILED", rc);
    fflush(stdout);
}
