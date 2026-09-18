// PondMerge v1 acceptance firmware.
//
// Which groups run depends on the part's static DRAM, and that is a compile-time
// decision expressed by PM_DEVICE_NO_SUITE (see main/CMakeLists.txt):
//
//   ESP32-S3 (256 KiB zone) : suite + concurrency + model
//   classic ESP32           : concurrency + model
//
// The suite is not skipped because it is slow or inconvenient. It is skipped
// because it cannot exist in less than a 256 KiB zone: test [10] asserts that
// 16 pools x 4 segments fills the zone, and test [2] creates a 32-segment pool,
// so the zone size is part of what those tests assert rather than a parameter
// they tolerate. A classic ESP32 has ~200 KiB of static DRAM in total, so the
// 256 KiB zone does not fit -- measured, not estimated: the link fails with
// "region `dram0_0_seg' overflowed by 126,744 bytes". Running the two groups
// that DO fit is worth doing; reporting the suite as passed because it was not
// compiled in would not be, so it is announced and counted as skipped.
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

#ifdef PM_DEVICE_NO_SUITE
#ifndef PM_TEST_ZONE_BYTES
#error "PM_DEVICE_NO_SUITE needs PM_TEST_ZONE_BYTES: this file owns the zone buffer"
#endif
// tests/suite.cpp normally provides this. Without the suite nothing else does,
// and both the concurrency test and the model borrow it (first 8 KiB and up to
// the whole buffer respectively).
uint8_t g_zone[PM_TEST_ZONE_BYTES] __attribute__((aligned(16)));
#endif

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

#ifdef PM_DEVICE_NO_SUITE
    // Deliberately loud, and deliberately not a "PASSED": a reader comparing
    // this log with the ESP32-S3 one must not be able to mistake a skipped group
    // for a passing one.
    printf("=== suite SKIPPED (not run, not passed) ===\n");
    printf("    this part has too little static DRAM for the suite's 256 KiB zone:\n");
    printf("    the link fails with \"dram0_0_seg overflowed by 126744 bytes\", and the\n");
    printf("    suite cannot be shrunk because its own assertions pin the zone to 64\n");
    printf("    segments (test [10]: 16 pools x 4 segments fills the zone; test [2]:\n");
    printf("    a 32-segment pool). Running concurrency + model, which need 8 KiB and\n");
    printf("    64 KiB. See tests/suite.cpp and esp32/main/CMakeLists.txt.\n");
    fflush(stdout);
#else
    int rc = pondmerge_run_tests(PM_DEVICE_STRESS_OPS);
    printf("=== suite %s (rc=%d) ===\n", rc == 0 ? "PASSED" : "FAILED", rc);
    fflush(stdout);
#endif

    int rc2 = pondmerge_run_concurrency_tests();
    printf("=== concurrency %s (rc=%d) ===\n", rc2 == 0 ? "PASSED" : "FAILED", rc2);
    fflush(stdout);

    // Reference-model differential on the device (round-5 guide section 7):
    // same fixed seed and public-API oracle as the host runner, 4000 ops
    // (the host scale for a 2000-op stress run), over the shared zone buffer.
    uint32_t mfail = pondmerge_run_model(64u * 1024u, 4000u);
    printf("=== model %s (rc=%u) ===\n", mfail == 0 ? "PASSED" : "FAILED",
           (unsigned)mfail);
    fflush(stdout);
}
