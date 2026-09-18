# The benchmark, the core, and the adapter. Paths climb out of bench/esp32/main
# to the repository root, the same way esp32/components/pondmerge points at
# ../../../src -- these are in-repo consumers, not the published entry point.
#
# PM_BENCH_NO_HOST_MAIN compiles out the benchmark's host main() so that
# app_main drives it instead. The benchmark source is otherwise UNCHANGED
# between host and device, which is what makes the two sets of numbers
# comparable.
idf_component_register(
    SRCS "bench_main.cpp"
         "../../../src/core.cpp"
         "../../alloc_latency.cpp"
    INCLUDE_DIRS "../../../include"
    PRIV_INCLUDE_DIRS "../../../src")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_ESP32
    # Sizing for a 512 KiB-SRAM part. PM_MAX_OBJECTS drives the static metadata
    # (~32 KiB at 256) and the zone below is the bench's own static array, so the
    # two together have to stay well inside internal SRAM.
    PM_MAX_OBJECTS=256
    PM_BENCH_NO_HOST_MAIN=1
    PM_BENCH_ZONE_BYTES=131072
    PM_BENCH_SEGMENT=4096
    PM_BENCH_TARGET_LIVE=256
    # The churn interval length sets how much of the instrument is amortised. On
    # the device the cycle counter is free, so this could be far smaller; it is
    # kept at the host value so the two runs have the same shape.
    PM_BENCH_CHURN_PAIRS=16384)
