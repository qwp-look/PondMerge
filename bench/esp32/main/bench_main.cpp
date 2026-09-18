// On-target driver for the PondMerge benchmarks.
//
// The benchmark itself is bench/alloc_latency.cpp, compiled unchanged; this file
// only reports the environment and calls it, so the numbers are produced by the
// same code that produces the host numbers.
//
// Watch it with a reader that never stops draining the port (tests/serial_cap.py
// or the snippet in examples/esp32_demo/README.md): the USB-Serial-JTAG console
// blocks printf once its transmit queue fills, which looks like "the test hung".
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "pondmerge/pm_config.h"

#include <cstdint>
#include <cstdio>

extern "C" void app_main(void);

// Defined in bench/alloc_latency.cpp (its host main() is compiled out by
// PM_BENCH_NO_HOST_MAIN).
int pm_bench_alloc_latency();

extern "C" void app_main(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    printf("\n=== PondMerge on-target benchmark ===\n");
    printf("chip: model=%d rev=%d.%d cores=%d\n", (int)info.model,
           info.revision >> 4, info.revision & 0xf, (int)info.cores);
    printf("PM_MAX_OBJECTS=%d PM_SL_COUNT=%d PM_FL_MAX=%d PM_DEBUG=%d\n",
           (int)PM_MAX_OBJECTS, (int)PM_SL_COUNT, (int)PM_FL_MAX, (int)PM_DEBUG);
    printf("free internal heap at boot: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    fflush(stdout);

    int const rc = pm_bench_alloc_latency();

    printf("=== benchmark done (rc=%d) ===\n", rc);
    fflush(stdout);
}
