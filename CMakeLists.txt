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
option(SB53_BUILD_QT    "Build the Qt 6 GUI"          OFF)  # superseded by the web UI
option(SB53_SANITIZE    "Address + UB sanitizers"     OFF)
option(SB53_COVERAGE    "Instrument for coverage"     OFF)
option(SB53_FUZZ        "Build libFuzzer targets"     OFF)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the C++ runtime statically on MSVC.
#
# Otherwise the executable needs VCRUNTIME140.dll and MSVCP140.dll, which are present on
# a developer machine but not necessarily on a printer host or a colleague's PC -- and the
# failure is a bare "the program can't start" dialog naming a DLL. A single self-contained
# binary is the whole point (ADR-0007).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
# Sanitizers, coverage, fuzzing
# ---------------------------------------------------------------------------
# All three are CI-only build modes. They change codegen substantially and must never be
# on for a binary anyone prints with.
if(SB53_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

if(SB53_COVERAGE AND NOT MSVC)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(SB53_FUZZ)
    # libFuzzer is clang-only. The parsers it targets consume untrusted input: G-code
    # arrives from a slicer but also from the internet, and the estimator's output
    # crosses a subprocess boundary.
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SB53_FUZZ requires clang")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -g)
    add_link_options(-fsanitize=address,undefined)
endif()

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

if(SB53_FUZZ)
    add_executable(fuzz_scanner tests/fuzz/fuzz_scanner.cpp)
    target_link_libraries(fuzz_scanner PRIVATE sb53::core)
    target_link_options(fuzz_scanner PRIVATE -fsanitize=fuzzer)
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
