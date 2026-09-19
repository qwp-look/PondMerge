# The real-scenario example (examples/sensor_pipeline.cpp) on a target.
# The source is UNCHANGED from the host build; only the sizing macros differ,
# which is what makes the two runs the same experiment. Sizing per target:
#
#   ESP32-S3     : 192 KiB zone, 240 history entries -- the descriptor table
#                  (PM_MAX_OBJECTS=256, set by the pondmerge component) is the
#                  binding constraint, not the zone.
#   classic ESP32: 64 KiB zone, 90 entries (the esp32_demo-proven sizing; the
#                  part has ~200 KiB of static DRAM in total).
#
# Both keep live objects <= ~250 against the 256-entry table, with headroom
# for the transient samples and the export batch.
#
# The export demand has a NARROW usable band on each target, and three sizing
# runs on the S3 measured it:
#
#   - At 8-16 KiB the demand always fits the pool's CONSOLIDATED TAIL (the
#     S3's object-table-capped occupancy is ~63%, leaving a ~60-70 KiB tail),
#     so the advice/compact flow never fires: 200 direct exports, zero
#     compactions. Dead code in its own demo -- at the host's 1200-tick flush
#     cadence AND at a 500-tick one (which only drained the pool further).
#   - At 48 KiB the demand exceeds what two ping-pong barriers let a
#     compaction deliver: the run records 193 direct / 1 rescued / 6 compacted
#     in vain -- "compact did not rescue this demand" is the honest dominant
#     outcome behind dense barriers (bench/RESULTS.md section 5.8), and a real
#     integrator MUST handle it, which export_batch() does.
#   - The host run (8 KiB against a 480-entry history, ~87% occupancy) sits
#     mid-band and shows the rescue path working: 13 rescued, 9 in vain,
#     8 skipped on advice over 200 exports.
#
# What fires is reported by the run itself, never promised here.
if(CONFIG_IDF_TARGET STREQUAL "esp32")
    set(PM_SP_ZONE_BYTES 65536)
    set(PM_SP_KEEP_MAX 90)
    set(PM_SP_EXPORT_BYTES 12288)
    set(PM_SP_FLUSH_EVERY 1200)
else()
    set(PM_SP_ZONE_BYTES 196608)
    set(PM_SP_KEEP_MAX 240)
    set(PM_SP_EXPORT_BYTES 49152)
    set(PM_SP_FLUSH_EVERY 1200)
endif()

idf_component_register(SRCS "app_main.cpp"
                            "../../sensor_pipeline.cpp"
                       REQUIRES pondmerge esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# PM_SENSOR_NO_HOST_MAIN compiles out the example's host main(); app_main
# drives it instead. The zone bytes and history cap are the ONLY differences
# from the host run. PM_MAX_OBJECTS must match the value the pondmerge
# component itself is built with (256, set via the root CMakeLists) so the
# static_assert guarding the descriptor table at sensor_pipeline.cpp:126
# checks against the REAL table size, not the 1024 default.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_SENSOR_NO_HOST_MAIN=1
    PM_MAX_OBJECTS=256
    PM_SP_ZONE_BYTES=${PM_SP_ZONE_BYTES}
    PM_SP_KEEP_MAX=${PM_SP_KEEP_MAX}
    PM_SP_EXPORT_BYTES=${PM_SP_EXPORT_BYTES}
    PM_SP_FLUSH_EVERY=${PM_SP_FLUSH_EVERY})
