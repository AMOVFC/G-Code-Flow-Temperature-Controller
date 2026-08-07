# sb53_core - the headless engine.
#
# THE ONE RULE (ADR-0002): this target links nothing that knows a user exists.
# No Qt, no windowing, no console I/O. It is verified by the `core-has-no-qt` test,
# not merely by convention.

add_library(sb53_core STATIC
    src/CurveCompare.cpp
    src/Diagnostics.cpp
    src/FlowAnalysis.cpp
    src/GcodeRewriter.cpp
    src/GcodeScanner.cpp
    src/Model.cpp
    src/TemperaturePlanner.cpp
    src/platform/SubprocessRunner.cpp
)

target_include_directories(sb53_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(sb53_core
    PRIVATE sb53_warnings sb53_werror
)

# Deliberately absent, and intended to stay absent:
#   find_package(Qt6 ...)
#   target_link_libraries(sb53_core ... Qt6::Core)
#
# If you find yourself wanting Qt here, the thing you want belongs behind one of the
# seams in Ports.hpp instead. See ARCHITECTURE.md, "The five seams".

set_target_properties(sb53_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(sb53::core ALIAS sb53_core)
