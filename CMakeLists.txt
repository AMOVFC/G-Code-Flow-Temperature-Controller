cmake_minimum_required(VERSION 3.24)

project(sb53
    VERSION 2.0.0
    DESCRIPTION "G-Code Flow & Temperature Controller"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SB53_BUILD_TESTS "Build the test suite"        ON)
option(SB53_BUILD_CLI   "Build the command-line tool" ON)
option(SB53_BUILD_QT    "Build the Qt 6 GUI"          OFF)  # enabled at milestone M8

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Floating-point determinism.
#
# We do NOT chase byte-parity with the legacy application (see ADR-0005), but we do
# require that our own results be reproducible across builds and machines. Fast-math
# and FMA contraction both allow the compiler to reassociate arithmetic, which makes
# results depend on optimisation level. Disable them.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()

include(cmake/SB53Warnings.cmake)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(core)

if(SB53_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(SB53_BUILD_QT)
    add_subdirectory(app)
endif()

if(SB53_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "sb53 ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests      : ${SB53_BUILD_TESTS}")
message(STATUS "  cli        : ${SB53_BUILD_CLI}")
message(STATUS "  qt gui     : ${SB53_BUILD_QT}")
message(STATUS "")
