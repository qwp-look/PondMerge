// On-target driver for the PondMerge benchmarks.
//
// The benchmarks themselves are bench/*.cpp, compiled UNCHANGED from the host
// build -- that is what makes the two sets of numbers comparable. This file
// supplies only what differs on a target: the scratch region they share, the
// environment and sizing report, and the sequencing between them.
// PM_BENCH_NO_HOST_MAIN compiles out each file's host main() so that app_main
// drives them instead.
//
// Watch it with a reader that never stops draining the port (tests/serial_cap.py
// or the snippet in examples/esp32_demo/README.md): the USB-Serial-JTAG console
// blocks printf once its transmit queue fills, which looks like "the test hung".
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "internal.h"
#include "pondmerge/pondmerge.hpp"
#include "pondmerge/pm_config.h"
#include "sdkconfig.h"

#include <cstdint>
#include <cstdio>

extern "C" void app_main(void);

// One scratch region, shared by all four benchmarks. The device runs them one
// after another, never at the same time, so a single region is enough -- and it
// has to be one, because four private regions of this size plus the PondMerge
// metadata do not fit a 512 KiB-SRAM part. The size comes from the same CMake
// variable that sets PM_BENCH_ZONE_BYTES / PM_BENCH_REGION_BYTES, so the region
// and the benchmarks' idea of it cannot drift apart.
//
// 16-byte aligned because pm::Config requires it; the benchmark files apply the
// same alignment to their host-side arrays.
alignas(16) uint8_t g_zone[PM_BENCH_ZONE_BYTES];

// Defined in the benchmark translation units.
int pm_bench_alloc_latency();
int pm_bench_churn_overhead();
int pm_bench_fragmentation();
int pm_bench_validate_scaling();
// Device-only: the FreeRTOS heap_4 baseline. It has no host counterpart, and it
// runs last because it re-uses g_zone as a heap region.
int pm_bench_idf_heap_baseline();

static void sizing_report(void) {
    using namespace pm::internal;
    // Every figure here is a sizeof() on the TARGET's ABI. The host formula
    // published in README.md was derived on x86-64, where a pointer is 8 bytes;
    // ObjectDesc holds one, so its size -- and therefore the closed form -- is
    // NOT the same on a 32-bit target. This section is how that is found out
    // rather than assumed.
    std::printf("--- sizing (target ABI) -----------------------------------------\n");
    std::printf("  sizeof(ObjectDesc)   : %u B\n", (unsigned)sizeof(ObjectDesc));
    std::printf("  sizeof(Pool)         : %u B\n", (unsigned)sizeof(Pool));
    std::printf("  sizeof(TlsfBins)     : %u B\n", (unsigned)sizeof(TlsfBins));
    std::printf("  sizeof(GlobalState)  : %u B   (PM_MAX_POOLS=%d,"
                " PM_MAX_OBJECTS=%d)\n",
                (unsigned)sizeof(GlobalState), (int)PM_MAX_POOLS,
                (int)PM_MAX_OBJECTS);
    std::printf("  metadata_scratch     : %u B   (maintenance plan scratch)\n",
                (unsigned)metadata_scratch_bytes());
    std::printf("  metadata_advice      : %u B   (compaction-advice state)\n",
                (unsigned)metadata_advice_bytes());
    std::printf("  metadata_bytes total : %u B\n",
                (unsigned)pm::global_stats().metadata_bytes);
    uint32_t const sum = (uint32_t)sizeof(GlobalState) +
                         metadata_scratch_bytes() + metadata_advice_bytes();
    std::printf("  sum of the three     : %u B  %s\n", (unsigned)sum,
                sum == pm::global_stats().metadata_bytes ? "(agrees)"
                                                         : "(MISMATCH)");
    std::printf("  pointer width        : %u B\n",
                (unsigned)sizeof(void*));
    std::printf("-----------------------------------------------------------------\n");
}

static void env_report(void) {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    std::printf("\n=== PondMerge on-target benchmarks ===\n");
    std::printf("chip                : model=%d rev=%d.%d cores=%d\n",
                (int)info.model, info.revision >> 4, info.revision & 0xf,
                (int)info.cores);
    std::printf("cpu frequency       : %d MHz (measured against mcycle)\n",
                (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    std::printf("PondMerge build     : PM_MAX_OBJECTS=%d PM_MAX_POOLS=%d"
                " PM_MAX_SEGMENTS=%d PM_SL_COUNT=%d PM_DEBUG=%d\n",
                (int)PM_MAX_OBJECTS, (int)PM_MAX_POOLS, (int)PM_MAX_SEGMENTS,
                (int)PM_SL_COUNT, (int)PM_DEBUG);
    std::printf("shared zone         : %u bytes (%u x %u B), one region for"
                " all four benchmarks\n",
                (unsigned)PM_BENCH_ZONE_BYTES,
                (unsigned)(PM_BENCH_ZONE_BYTES / PM_BENCH_SEGMENT),
                (unsigned)PM_BENCH_SEGMENT);
    std::printf("free internal heap  : %u bytes (8-bit capable)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT));
    std::printf("PM_DEBUG=0 means this run has RELEASE semantics: the debug"
                " assertions that the\nacceptance suite exercises are compiled"
                " out, exactly as in the host Release build.\n");
    sizing_report();
    std::fflush(stdout);
}

extern "C" void app_main(void) {
    env_report();

    // pm::init() is once-per-instance and none of the benchmarks calls deinit(),
    // because on the host each is its own process. Chained in one app_main they
    // are not, so the driver shuts the instance down between them -- and
    // deinit() REFUSES while any object is live, which is how a benchmark that
    // forgets to clean up becomes an immediate "init failed" in the next one
    // rather than a quietly wrong measurement. (That is not hypothetical: it is
    // what happened on the first two runs of this file.)
    int rc = 0;
    struct Step {
        const char* title;
        int (*fn)();
    };
    Step const steps[] = {
        {"1/5 alloc latency", pm_bench_alloc_latency},
        {"2/5 churn harness decomposition", pm_bench_churn_overhead},
        {"3/5 fragmentation A/B", pm_bench_fragmentation},
        {"4/5 validate / get_stats scaling", pm_bench_validate_scaling},
        // Last, because it takes g_zone over as a heap region once PondMerge is
        // done with it. See idf_heap_baseline.cpp.
        {"5/5 baseline: ESP-IDF own heap (TLSF)", pm_bench_idf_heap_baseline},
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        std::printf("\n########## %s ##########\n", steps[i].title);
        std::fflush(stdout);
        rc |= steps[i].fn();
        pm::Status const st = pm::deinit();
        std::printf("[driver] %s done; deinit -> %s\n", steps[i].title,
                    pm::status_name(st));
        if (st == pm::Status::Busy) {
            std::printf("[driver] BUSY means the step left live objects behind;"
                        " the next init will fail.\n");
        } else if (st == pm::Status::InvalidPool) {
            std::printf("[driver] INVALID_POOL here means the step deinit'ed the"
                        " instance itself. Fine.\n");
        }
        std::fflush(stdout);
    }

    std::printf("\n=== benchmarks done (rc=%d) ===\n", rc);
    std::fflush(stdout);
}
