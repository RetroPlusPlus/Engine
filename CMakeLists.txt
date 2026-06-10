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

# ── SDL3 (vendored submodule) ──────────────────────────────────────────────────
# The project-locked platform boundary. Built statically from source so the engine
# links one self-contained binary (matching the build-modes single-binary posture);
# consumers inherit it transitively through gbcpp::engine. Subsystems are trimmed to
# what the platform layer needs now — video + events + gamepad; the audio backend
# lands with the audio chain (ENG-4). Pinned in .gitmodules (release-3.4.10). Added
# SYSTEM so SDL's headers don't trip the engine's -Wpedantic bar.
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO        OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA       OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC       OFF CACHE BOOL "" FORCE)
set(SDL_POWER        OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG       OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sdl EXCLUDE_FROM_ALL SYSTEM)

# ── Shipped engine target ───────────────────────────────────────────────────────
# Static library consumers link. The gbcpp::engine alias is the surface consumers
# link against — a missing target then fails at configure time with a clear name.
# The platform layer (SdlPlatform) is compiled on every platform so its live SDL_GPU
# usage cannot silently rot; the live device is exercised by the window demo, never
# in CI (CI runs the headless suites against MockPlatform).
add_library(gbcppengine STATIC
    src/version.cpp
    src/clock.cpp
    src/input.cpp
    src/run_loop.cpp
    src/input_map.cpp
    src/windowed_host.cpp
    src/sdl_platform.cpp
    src/draw_state.cpp
    src/renderer.cpp)
add_library(gbcpp::engine ALIAS gbcppengine)
target_include_directories(gbcppengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Private: the renderer #includes the generated shader headers under shaders/generated/.
# Not part of the public interface — consumers never see them.
target_include_directories(gbcppengine PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_features(gbcppengine PUBLIC cxx_std_20)
# PUBLIC: SdlPlatform's headers expose SDL types, and consumers inherit SDL when they
# link the engine (Decision #2).
target_link_libraries(gbcppengine PUBLIC SDL3::SDL3)
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

    add_executable(gbcppengine-tests
        tests/smoke_test.cpp
        tests/run_loop_test.cpp
        tests/input_test.cpp
        tests/double_buffer_test.cpp
        tests/input_map_test.cpp
        tests/controls_test.cpp
        tests/windowed_host_test.cpp
        tests/geometry_test.cpp
        tests/viewport_test.cpp
        tests/shader_format_test.cpp
        tests/draw_state_test.cpp
        tests/tilemap_test.cpp)
    target_compile_features(gbcppengine-tests PRIVATE cxx_std_20)
    # tests/ on the include path so test-only helpers (manual_clock.h) resolve.
    target_include_directories(gbcppengine-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
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

# ── Runnable example hosts (mode 1 only by default) ──────────────────────────────
# The window demo is the manual runtime check for the live platform path (open a
# window, present a cleared frame, route real input). It is built — so its use of the
# live SdlPlatform compiles and links on every CI platform — but never run in CI,
# which has no display. Off for consumers (built only when the engine is top-level).
option(GBCPP_BUILD_EXAMPLES "Build runnable example hosts" ${PROJECT_IS_TOP_LEVEL})
if(GBCPP_BUILD_EXAMPLES)
    add_executable(gbcpp-window-demo examples/window_demo.cpp)
    target_compile_features(gbcpp-window-demo PRIVATE cxx_std_20)
    target_link_libraries(gbcpp-window-demo PRIVATE gbcpp::engine)
    gbcpp_set_warnings(gbcpp-window-demo)
endif()
