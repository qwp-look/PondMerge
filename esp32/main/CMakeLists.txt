idf_component_register(SRCS "main.cpp" "../../tests/suite.cpp"
                       INCLUDE_DIRS "../../tests" "../../include" "../components/pondmerge")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_MAX_OBJECTS=256)
