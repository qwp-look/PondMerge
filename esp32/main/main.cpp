// PondMerge v1 acceptance firmware.
//
// Since v20 the suite's fixture geometry is a compile-time knob
// (PM_TEST_SEG_BYTES x 64 segments, see main/CMakeLists.txt), and every
// supported target builds the FULL suite + concurrency + model: at 1 KiB
// segments the 64 KiB zone fits the static DRAM of the classic ESP32 too,
// which previously had to run with the suite compiled out (its 256 KiB zone
// did not link: "region `dram0_0_seg' overflowed by 126,744 bytes").
//
// The suites live in tests/suite.cpp, tests/concurrency_esp32.cpp and
// tests/model.cpp.
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdint>

extern "C" void app_main(void);
int pondmerge_run_tests(uint32_t stress_ops);
int pondmerge_run_concurrency_tests(void);
uint32_t pondmerge_run_model(uint32_t zone_bytes, uint32_t ops);

#ifndef PM_DEVICE_STRESS_OPS
#define PM_DEVICE_STRESS_OPS 2000
#endif

// The fixture geometry, mirroring tests/suite.cpp's derivation (the CMake
// passes the same PM_TEST_SEG_BYTES to every translation unit).
#ifndef PM_TEST_SEG_BYTES
#define PM_TEST_SEG_BYTES 1024u
#endif
#define PM_TEST_ZONE_BYTES (64u * PM_TEST_SEG_BYTES)

extern "C" void app_main(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    printf("\n=== PondMerge v1 acceptance firmware ===\n");
    printf("chip: model=%d rev=%d.%d cores=%d\n", (int)info.model,
           info.revision >> 4, info.revision & 0xf, (int)info.cores);
    printf("zone: %u bytes  stress ops: %d (override with -DPM_DEVICE_STRESS_OPS=)\n",
           (unsigned)PM_TEST_ZONE_BYTES, (int)PM_DEVICE_STRESS_OPS);
    printf("free internal heap at boot: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fflush(stdout);

    int rc = pondmerge_run_tests(PM_DEVICE_STRESS_OPS);
    printf("=== suite %s (rc=%d) ===\n", rc == 0 ? "PASSED" : "FAILED", rc);
    fflush(stdout);

    int rc2 = pondmerge_run_concurrency_tests();
    printf("=== concurrency %s (rc=%d) ===\n", rc2 == 0 ? "PASSED" : "FAILED", rc2);
    fflush(stdout);

    // Reference-model differential on the device (round-5 guide section 7):
    // same fixed seed and public-API oracle as the host runner, 4000 ops
    // (the host scale for a 2000-op stress run), over the shared zone buffer.
    uint32_t mfail = pondmerge_run_model(PM_TEST_ZONE_BYTES, 4000u);
    printf("=== model %s (rc=%u) ===\n", mfail == 0 ? "PASSED" : "FAILED",
           (unsigned)mfail);
    fflush(stdout);
}
