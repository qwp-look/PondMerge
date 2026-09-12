idf_component_register(SRCS "demo_main.cpp"
                       REQUIRES pondmerge esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
execute_process(COMMAND git -C ${CMAKE_CURRENT_LIST_DIR}/../../.. rev-parse --short=7 HEAD
                OUTPUT_VARIABLE PM_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT PM_COMMIT)
    set(PM_COMMIT "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_DEMO_COMMIT="${PM_COMMIT}")
