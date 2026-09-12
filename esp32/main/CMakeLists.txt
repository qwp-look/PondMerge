idf_component_register(SRCS "main.cpp" "../../tests/suite.cpp"
                            "../../tests/concurrency_esp32.cpp"
                            "../../tests/model.cpp"
                       INCLUDE_DIRS "../../tests" "../../include" "../components/pondmerge")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# PM_DEVICE_BUILD: model.cpp reuses the suite's zone buffer instead of its own
# static one (device DRAM budget), and main.cpp sizes the model run for the
# device (round-5 guide section 7).
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_MAX_OBJECTS=256 PM_DEVICE_BUILD=1)
