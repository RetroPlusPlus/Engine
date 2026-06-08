cmake_minimum_required(VERSION 3.28)
project(gbcpp-engine VERSION 0.1.0 LANGUAGES CXX)

# ── Build modes ────────────────────────────────────────────────────────────────
# A single root build serves three modes; the active mode follows from whether
# the engine is the top-level project and one cache option:
#
#   1. Engine alone        — engine is top-level (PROJECT_IS_TOP_LEVEL). Builds the
#                            engine + testkit + the engine's own tests. This is what
#                            the engine's standalone CI exercises.
#   2. Consumer data-level — engine not referenced at all. Nothing here builds; a
#                            consumer's engine-free data/test build links zero engine
#                            code. This build must never break that posture.
#   3. Engine + consumer   — a consumer calls add_subdirectory(engine). The shipped
#                            gbcpp::engine target is exposed for linking; the consumer
#                            opts into gbcpp::testkit for fidelity-harness tests. The
#                            engine's own tests are OFF by default when not top-level.
#
# Consumers that want harness tests link gbcpp::testkit regardless of this option;
# the testkit target is always defined.
option(GBCPP_BUILD_TESTS "Build the engine's own tests" ${PROJECT_IS_TOP_LEVEL})

# Emit build/compile_commands.json for clangd and any other LSP consuming the
# database. The .clangd file at the repo root points clangd at it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── ccache (optional build cache) ─────────────────────────────────────────────
# No-ops cleanly where ccache is absent; reuses the CI hosts' existing cache.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# ── MSVC: correct __cplusplus value ───────────────────────────────────────────
if(MSVC)
    add_compile_options(/Zc:__cplusplus)
endif()

# ── Compiler floors ────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
    message(FATAL_ERROR "GCC 13+ required (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.0")
    message(FATAL_ERROR "Clang 16+ required (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.38")
    message(FATAL_ERROR "MSVC 19.38+ (Visual Studio 2022 17.8+) required")
endif()

# ── Warning flags (clean-warnings bar from day one) ────────────────────────────
function(gbcpp_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ── Shipped engine target ───────────────────────────────────────────────────────
# Static library consumers link. One real translation unit at ENG-0 (version.cpp)
# so the library is non-empty; the run loop / renderer / VM / audio bodies land in
# their own sub-blocks (ENG-1+). The gbcpp::engine alias is the surface consumers
# link against — a missing target then fails at configure time with a clear name.
add_library(gbcppengine STATIC src/version.cpp)
add_library(gbcpp::engine ALIAS gbcppengine)
target_include_directories(gbcppengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(gbcppengine PUBLIC cxx_std_20)
gbcpp_set_warnings(gbcppengine)

# ── Test-tooling target ──────────────────────────────────────────────────────────
# The fidelity-harness wrapper's home. Always defined (so the link surface exists
# in every mode), but NEVER linked into a shipped game binary — only into test
# executables. A placeholder TU at ENG-0; the full SameBoy-core wrapper + scenario
# base class land at ENG-7.
add_library(gbcpp-testkit STATIC testkit/src/testkit_stub.cpp)
add_library(gbcpp::testkit ALIAS gbcpp-testkit)
target_include_directories(gbcpp-testkit PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/testkit/include>)
target_compile_features(gbcpp-testkit PUBLIC cxx_std_20)
gbcpp_set_warnings(gbcpp-testkit)

# ── Engine tests (mode 1 only by default) ────────────────────────────────────────
# GoogleTest is fetched only inside this branch, so a mode-3 consumer building
# without engine tests pulls no test dependency.
if(GBCPP_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(gbcppengine-tests tests/smoke_test.cpp)
    target_compile_features(gbcppengine-tests PRIVATE cxx_std_20)
    # Link the NAMESPACED aliases exactly as a mode-3 consumer would. This test
    # executable IS the in-tree consumer fixture for the link surface: it proves
    # both gbcpp::engine and gbcpp::testkit resolve as aliases and link cleanly.
    target_link_libraries(gbcppengine-tests PRIVATE
        gbcpp::engine
        gbcpp::testkit
        GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(gbcppengine-tests)
endif()
