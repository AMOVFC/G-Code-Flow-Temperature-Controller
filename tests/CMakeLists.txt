include(FetchContent)

# Prefer an already-installed Catch2 so the build works offline; fall back to fetching
# a pinned release.
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2          # pinned deliberately; bump as its own commit
    FIND_PACKAGE_ARGS 3 NAMES Catch2
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
# Hermetic and fast: no subprocesses, no filesystem fixtures, no estimator binary.
# These assert specification invariants rather than recorded values, so they survive
# refactoring. See ADR-0005.

add_executable(sb53_unit_tests
    unit/test_diagnostics.cpp
    unit/test_model.cpp
    unit/test_scanner.cpp
)

target_link_libraries(sb53_unit_tests
    PRIVATE sb53::core sb53_warnings Catch2::Catch2WithMain
)

# Fixture location. Committed fixtures under testdata/fixtures/ are always present;
# full-size files under testdata/reference/ are git-ignored, so tests using those must
# skip gracefully when absent (see testdata/README.md).
target_compile_definitions(sb53_unit_tests
    PRIVATE SB53_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata"
)

catch_discover_tests(sb53_unit_tests)

# ---------------------------------------------------------------------------
# Architectural guard
# ---------------------------------------------------------------------------
add_test(NAME core-has-no-qt
    COMMAND ${CMAKE_COMMAND}
            -DSB53_CORE_DIR=${CMAKE_SOURCE_DIR}/core
            -P ${CMAKE_SOURCE_DIR}/cmake/CheckNoQt.cmake
)
