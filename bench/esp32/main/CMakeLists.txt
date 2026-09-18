# The four benchmarks, the core, and the adapter. Paths climb out of
# bench/esp32/main to the repository root, the same way esp32/components/pondmerge
# points at ../../../src -- these are in-repo consumers, not the published entry
# point.
#
# The benchmark sources are UNCHANGED between host, ESP32-S3 and classic ESP32;
# only the sizing macros below differ, which is what makes the three sets of
# numbers comparable. To build the host numbers for comparison, see bench/README.md.

# --- target sizing -----------------------------------------------------------
# The shared region is ONE array that all four benchmarks take turns using, so its
# size is what decides whether this firmware runs on a given part at all. That is
# the only reason the two targets differ here, and every other benchmark parameter
# is deliberately left identical so the second chip produces a comparable point
# rather than a differently-shaped experiment.
#
#   ESP32-S3     : 512 KiB SRAM; .bss comes out at 241,056 B, which it holds.
#   classic ESP32: ~200 KiB of static DRAM in TOTAL (dram0_0_seg = 204,800 B), so
#                  the S3's 192 KiB region does not fit -- and neither does
#                  128 KiB: the link failed with "dram0_0_seg overflowed by 4208
#                  bytes". 112 KiB is what is used here, chosen as a round number
#                  that leaves ~12 KiB of margin rather than shaving the last few
#                  KB off a region that has to coexist with the IDF runtime.
#
# POPULATION is an upper bound, not a target -- the fill loop stops when the
# region is exhausted -- but it has to be at least as large as what the region
# holds, and a 112 KiB region holds ~112 objects of this size mix against ~191 in
# 192 KiB. Everything downstream of the fill (probe size, pin cadence, scatter
# cadence, probe cadence) is the same on both targets on purpose.
if(CONFIG_IDF_TARGET STREQUAL "esp32")
    set(PM_BENCH_SHARED_ZONE_BYTES 114688)  # 112 KiB: one region for all four
    set(PM_BENCH_POPULATION 128)
    set(PM_BENCH_CW_EVENTS 128)
else()
    set(PM_BENCH_SHARED_ZONE_BYTES 196608)  # 192 KiB
    set(PM_BENCH_POPULATION 192)
    set(PM_BENCH_CW_EVENTS 512)
endif()
set(PM_BENCH_SEGMENT 4096)
# Derived, not hand-maintained: the pool claims the whole region, and a segment
# count that disagrees with the region size is a silent misconfiguration.
math(EXPR PM_BENCH_SEGMENTS "${PM_BENCH_SHARED_ZONE_BYTES} / ${PM_BENCH_SEGMENT}")

idf_component_register(
    SRCS "bench_main.cpp"
         "idf_heap_baseline.cpp"
         "perfcount.cpp"
         "../../../src/core.cpp"
         "../../alloc_latency.cpp"
         "../../churn_overhead.cpp"
         "../../compaction_window.cpp"
         "../../fragmentation.cpp"
         "../../validate_scaling.cpp"
    INCLUDE_DIRS "../../../include"
    PRIV_INCLUDE_DIRS "../../../src"
    # esp_timer for pm_port.h's tick source, freertos for the portMUX spinlock,
    # heap for the heap_caps_* baseline, perfmon for the Xtensa performance
    # counters that perfcount.cpp reads. bench_main.cpp reaches two of them
    # directly; core.cpp reaches none, so they are requirements of the whole
    # component.
    PRIV_REQUIRES esp_timer freertos heap perfmon)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_ESP32
    PM_MAX_OBJECTS=256
    # Release semantics: the host figures these runs are compared against come
    # from a -O2 -DNDEBUG -DPM_DEBUG=0 build, and the acceptance firmware is the
    # one that exercises the debug assertions.
    PM_DEBUG=0
    PM_BENCH_NO_HOST_MAIN=1
    # One shared region: the benchmarks declare the region extern and this file
    # defines it, at the size set above. Both numbers come from the same variable
    # so they cannot drift.
    PM_BENCH_SHARED_ZONE=1
    PM_BENCH_ZONE_BYTES=${PM_BENCH_SHARED_ZONE_BYTES}
    PM_BENCH_REGION_BYTES=${PM_BENCH_SHARED_ZONE_BYTES}
    PM_BENCH_SEGMENT=${PM_BENCH_SEGMENT}
    PM_BENCH_SEGMENTS=${PM_BENCH_SEGMENTS}
    # --- per-benchmark sizing (identical on both targets unless noted) ---
    # alloc latency: sample up to 256 live objects (PM_MAX_OBJECTS caps it), and
    # keep CHURN_PAIRS at the host value so the runs have the same shape. These
    # three are the numbers the resident-object curves are plotted against, so
    # they must not differ between chips.
    PM_BENCH_TARGET_LIVE=256
    PM_BENCH_LIVE_MAX=256
    PM_BENCH_CHURN_PAIRS=16384
    # churn decomposition: the live count and interval length only have to be
    # representative, and OPS is smaller than CHURN_PAIRS on purpose -- four rows
    # x 8 trials at a few tens of ms each keeps the whole section inside seconds.
    PM_BENCH_CHURN_LIVE=256
    PM_BENCH_CHURN_OPS=4096
    # fragmentation AND the IDF-heap baseline. POPULATION is the only one of
    # these that is target-dependent (see above); PROBE_BYTES stays at 10 KiB on
    # both because it has to be larger than the biggest hole a 2 KiB object leaves
    # behind (so variant A fails) and smaller than the distance between pin
    # fence posts (so variant B can still be rescued by compacting) -- both of
    # which hold at 128 KiB with ~7 pins as well as at 192 KiB with ~11.
    PM_BENCH_POPULATION=${PM_BENCH_POPULATION}
    PM_BENCH_PROBE_BYTES=10240
    PM_BENCH_STEPS=40000
    PM_BENCH_PROBE_EVERY=200
    PM_BENCH_PINNED_EVERY=16
    PM_BENCH_SCATTER_MOD=4
    PM_BENCH_PHASE1_TRIALS=50
    # compaction window: 512 recorded events on the S3 (16 B of sample state
    # each, ~12 KiB including the report arrays -- the S3 has the headroom).
    # The classic part's .bss margin is ~12 KiB in TOTAL, so it records 128
    # events (~3 KiB) -- enough for a coarse distribution, not for a p99.
    PM_BENCH_CW_EVENTS=${PM_BENCH_CW_EVENTS})
