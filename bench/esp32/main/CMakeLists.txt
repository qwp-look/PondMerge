# The four benchmarks, the core, and the adapter. Paths climb out of
# bench/esp32/main to the repository root, the same way esp32/components/pondmerge
# points at ../../../src -- these are in-repo consumers, not the published entry
# point.
#
# The benchmark sources are UNCHANGED between host and device; only the sizing
# macros below differ, which is what makes the two sets of numbers comparable.
# To build the host numbers for comparison, see bench/README.md.

# --- target sizing -----------------------------------------------------------
# 512 KiB-SRAM part. PM_MAX_OBJECTS drives the static metadata and the shared
# zone is a 192 KiB array; the two together plus the IDF runtime come to
# ~232 KiB of .bss, which is what the linker reported holding.
set(PM_BENCH_SHARED_ZONE_BYTES 196608)  # 192 KiB: one region for all four
set(PM_BENCH_SEGMENT 4096)

idf_component_register(
    SRCS "bench_main.cpp"
         "idf_heap_baseline.cpp"
         "../../../src/core.cpp"
         "../../alloc_latency.cpp"
         "../../churn_overhead.cpp"
         "../../fragmentation.cpp"
         "../../validate_scaling.cpp"
    INCLUDE_DIRS "../../../include"
    PRIV_INCLUDE_DIRS "../../../src"
    # esp_timer for pm_port.h's tick source, freertos for the portMUX spinlock,
    # heap for the heap_caps_* baseline. bench_main.cpp reaches the last one
    # directly; core.cpp does not, so heap is a private requirement of the whole
    # component.
    PRIV_REQUIRES esp_timer freertos heap)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_ESP32
    PM_MAX_OBJECTS=256
    # Release semantics: the host figures this run is compared against come from
    # a -O2 -DNDEBUG -DPM_DEBUG=0 build, and the acceptance firmware is the one
    # that exercises the debug assertions.
    PM_DEBUG=0
    PM_BENCH_NO_HOST_MAIN=1
    # One shared region: the benchmarks declare the region extern and this file
    # defines it, at the size set above. Both numbers come from the same variable
    # so they cannot drift.
    PM_BENCH_SHARED_ZONE=1
    PM_BENCH_ZONE_BYTES=${PM_BENCH_SHARED_ZONE_BYTES}
    PM_BENCH_REGION_BYTES=${PM_BENCH_SHARED_ZONE_BYTES}
    PM_BENCH_SEGMENT=${PM_BENCH_SEGMENT}
    PM_BENCH_SEGMENTS=48
    # --- per-benchmark sizing ---
    # alloc latency: sample up to 256 live objects (PM_MAX_OBJECTS caps it), and
    # keep CHURN_PAIRS at the host value so the two runs have the same shape.
    PM_BENCH_TARGET_LIVE=256
    PM_BENCH_LIVE_MAX=256
    PM_BENCH_CHURN_PAIRS=16384
    # churn decomposition: the live count and interval length only have to be
    # representative, and OPS is smaller than CHURN_PAIRS on purpose -- four rows
    # x 8 trials at ~40 ms each keeps the whole section inside a few seconds.
    PM_BENCH_CHURN_LIVE=256
    PM_BENCH_CHURN_OPS=4096
    # fragmentation AND the heap_4 baseline, from ONE place so the two cannot
    # drift apart -- the baseline is only meaningful if it runs the same load.
    # A 192 KiB region fills with ~170 objects of the steep size mix, so 192 is an
    # upper bound, not a target: the fill loop stops when the region is exhausted.
    # The probe stays at 10 KiB, well above the largest hole a 2 KiB object leaves
    # behind, so it still fails without compaction.
    PM_BENCH_POPULATION=192
    PM_BENCH_PROBE_BYTES=10240
    PM_BENCH_STEPS=40000
    PM_BENCH_PROBE_EVERY=200
    PM_BENCH_PINNED_EVERY=16
    PM_BENCH_SCATTER_MOD=4
    PM_BENCH_PHASE1_TRIALS=50)
