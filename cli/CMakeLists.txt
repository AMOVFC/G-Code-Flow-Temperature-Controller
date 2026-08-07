add_executable(sb53-cli main.cpp)

target_link_libraries(sb53-cli
    PRIVATE sb53::core sb53_warnings
)

set_target_properties(sb53-cli PROPERTIES
    OUTPUT_NAME "sb53"
)
