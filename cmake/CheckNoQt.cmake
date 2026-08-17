# Enforces ADR-0002: core/ must not depend on Qt.
#
# Run as:  cmake -DSB53_CORE_DIR=<path> -P cmake/CheckNoQt.cmake
#
# This is a test, not a lint. The core being UI-agnostic is the property that makes the
# planned web frontend a routing layer rather than a rewrite; if it erodes silently, the
# erosion is only discovered when the second frontend is attempted and it is far too
# late to fix cheaply.

if(NOT DEFINED SB53_CORE_DIR)
    message(FATAL_ERROR "SB53_CORE_DIR must be set")
endif()

file(GLOB_RECURSE sources
    "${SB53_CORE_DIR}/*.cpp"
    "${SB53_CORE_DIR}/*.hpp"
    "${SB53_CORE_DIR}/*.h"
)

set(violations "")

foreach(file ${sources})
    # Skip vendored third-party sources; we do not control their includes.
    if(file MATCHES "/third_party/")
        continue()
    endif()

    file(READ "${file}" contents)

    # Qt headers, the Q_OBJECT macro, and qDebug all indicate a Qt dependency.
    if(contents MATCHES "#[ \t]*include[ \t]*[<\"]Q" OR
       contents MATCHES "Q_OBJECT" OR
       contents MATCHES "qDebug[ \t]*\\(")
        list(APPEND violations "${file}")
    endif()
endforeach()

if(violations)
    message("")
    message("core/ must not depend on Qt (ADR-0002). Offending files:")
    foreach(v ${violations})
        message("  ${v}")
    endforeach()
    message("")
    message("If you need platform or UI services in core, add a seam in Ports.hpp")
    message("and implement it in the frontend instead.")
    message("")
    message(FATAL_ERROR "core-has-no-qt failed")
endif()

list(LENGTH sources source_count)
message(STATUS "core-has-no-qt: OK (${source_count} sources scanned)")
