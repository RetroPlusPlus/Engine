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

# ── Build-time shader generation (per-platform native toolchain) ──────────────
# Every build compiles shaders/src/*.hlsl to the ONE format this platform's SDL_GPU
# backend runs, using the platform's native tools — nothing is cross-built and no
# bytecode is committed. gen_shader.py emits headers into the build tree under the
# exact include path the renderer already uses (shaders/generated/<stem>.h), with
# the same symbol surface as before; formats not built here are nullptr constants
# selectShader treats as absent.
#
#   macOS   → MSL    (glslang HLSL→SPIR-V, spirv-cross SPIR-V→MSL)  brew install glslang spirv-cross
#   Linux   → SPIR-V (glslang HLSL→SPIR-V)                          apt install glslang-tools
#   Windows → DXIL   (dxc, ships with the Windows SDK)
if(APPLE)
    set(GBCPP_SHADER_FORMAT msl)
elseif(WIN32)
    set(GBCPP_SHADER_FORMAT dxil)
else()
    set(GBCPP_SHADER_FORMAT spirv)
endif()

set(GBCPP_SHADER_TOOL_ARGS "")
if(GBCPP_SHADER_FORMAT STREQUAL "dxil")
    # dxc lives in the Windows SDK bin dir, which is not on PATH outside a VS
    # developer prompt — search the installed SDK versions, host arch first,
    # newest SDK first. SDKs install tool dirs for FOREIGN architectures too, so
    # mere existence is not enough: every candidate is validated by running it
    # (an arm64 dxc.exe on an x64 host exists but cannot execute — observed on
    # the x64 CI runner, which selected it and died with "unknown error").
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_gbcpp_dxc_arch arm64)
    else()
        set(_gbcpp_dxc_arch x64)
    endif()
    file(GLOB _gbcpp_sdk_bins
        "C:/Program Files (x86)/Windows Kits/10/bin/10.*/${_gbcpp_dxc_arch}")
    list(SORT _gbcpp_sdk_bins ORDER DESCENDING)
    function(gbcpp_dxc_runs out_var candidate)
        execute_process(COMMAND "${candidate}" --version
                        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _rc EQUAL 0)
            set(${out_var} FALSE PARENT_SCOPE)
        endif()
    endfunction()
    find_program(GBCPP_DXC NAMES dxc
                 HINTS ${_gbcpp_sdk_bins}
                 VALIDATOR gbcpp_dxc_runs)
    if(NOT GBCPP_DXC)
        message(FATAL_ERROR "no runnable dxc found — install the Windows SDK's "
                            "${_gbcpp_dxc_arch} build tools (ships dxc) or put a "
                            "native dxc on PATH")
    endif()
    list(APPEND GBCPP_SHADER_TOOL_ARGS "-DDXC=${GBCPP_DXC}")
else()
    # HINTS cover Homebrew's install prefixes for runner services whose PATH
    # doesn't include them (launchd services commonly ship a minimal PATH).
    find_program(GBCPP_GLSLANG NAMES glslang glslangValidator
                 HINTS /opt/homebrew/bin /usr/local/bin)
    if(NOT GBCPP_GLSLANG)
        message(FATAL_ERROR "glslang not found — brew install glslang / apt install glslang-tools")
    endif()
    list(APPEND GBCPP_SHADER_TOOL_ARGS "-DGLSLANG=${GBCPP_GLSLANG}")
    if(GBCPP_SHADER_FORMAT STREQUAL "msl")
        find_program(GBCPP_SPIRV_CROSS NAMES spirv-cross
                     HINTS /opt/homebrew/bin /usr/local/bin)
        if(NOT GBCPP_SPIRV_CROSS)
            message(FATAL_ERROR "spirv-cross not found — brew install spirv-cross")
        endif()
        list(APPEND GBCPP_SHADER_TOOL_ARGS "-DSPIRV_CROSS=${GBCPP_SPIRV_CROSS}")
    endif()
endif()

set(GBCPP_SHADER_STEMS blit.vert blit.frag tile.vert tile.frag sprite.vert sprite.frag)
set(_gbcpp_shader_headers "")
foreach(_stem IN LISTS GBCPP_SHADER_STEMS)
    string(REPLACE "." "_" _ns "${_stem}")
    string(REGEX MATCH "[^.]+$" _stage "${_stem}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/shaders/generated/${_ns}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}"
                -DFORMAT=${GBCPP_SHADER_FORMAT} -DSTAGE=${_stage} -DSTEM=${_stem}
                "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/shaders/src/${_stem}.hlsl"
                "-DOUT=${_out}"
                "-DTMP=${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/tmp"
                ${GBCPP_SHADER_TOOL_ARGS}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_shader.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/src/${_stem}.hlsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_shader.cmake"
        COMMENT "Generating ${GBCPP_SHADER_FORMAT} shader header ${_ns}.h"
        VERBATIM)
    list(APPEND _gbcpp_shader_headers "${_out}")
endforeach()
add_custom_target(gbcpp-shaders DEPENDS ${_gbcpp_shader_headers})

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
# They are generated into the build tree (BEFORE keeps that copy authoritative) by the
# gbcpp-shaders target. Not part of the public interface — consumers never see them.
target_include_directories(gbcppengine BEFORE PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated-shaders>)
target_include_directories(gbcppengine PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
add_dependencies(gbcppengine gbcpp-shaders)
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
        tests/timing_test.cpp
        tests/input_test.cpp
        tests/double_buffer_test.cpp
        tests/input_map_test.cpp
        tests/input_profile_test.cpp
        tests/engine_config_test.cpp
        tests/controls_test.cpp
        tests/windowed_host_test.cpp
        tests/geometry_test.cpp
        tests/viewport_test.cpp
        tests/shader_format_test.cpp
        tests/draw_state_test.cpp
        tests/tilemap_test.cpp
        tests/palette_test.cpp
        tests/tile_attributes_test.cpp
        tests/sprite_test.cpp
        tests/frame_finish_test.cpp)
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
