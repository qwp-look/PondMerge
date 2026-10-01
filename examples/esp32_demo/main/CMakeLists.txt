idf_component_register(SRCS "demo_main.cpp"
                       REQUIRES pondmerge esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# PM_MAX_OBJECTS comes from the pondmerge component's PUBLIC definitions
# (256 on device) -- any PM_MAX_OBJECTS-derived static_assert in the demo
# source checks against the REAL table size automatically. Do not add a
# private -D copy here: it would override the propagated value.
execute_process(COMMAND git -C ${CMAKE_CURRENT_LIST_DIR}/../../.. rev-parse --short=7 HEAD
                OUTPUT_VARIABLE PM_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT PM_COMMIT)
    set(PM_COMMIT "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_DEMO_COMMIT="${PM_COMMIT}")
