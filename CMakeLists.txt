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

# ── lodepng (vendored single-file PNG decoder) ────────────────────────────────
# Image ingestion's decoder (ENG-2.B.3.a). Built as its own static library so the
# engine's strict -Wall/-Wextra/-Wpedantic bar never applies to third-party source,
# and its header is SYSTEM so including it from image.cpp doesn't trip -Wpedantic.
# Linked PRIVATE into the engine: lodepng symbols never reach a public header, and a
# consumer links it transitively through the static engine lib. Pinned in
# third_party/lodepng/ (upstream commit ed6fe58, zlib/MIT). No submodule, no own build.
add_library(gbcpp-lodepng STATIC third_party/lodepng/lodepng.cpp)
target_include_directories(gbcpp-lodepng SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/lodepng>)
target_compile_features(gbcpp-lodepng PUBLIC cxx_std_20)

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

# Generate ONE HLSL shader to this platform's bytecode header (shaders/generated/<ns>.h in the build
# tree). The engine calls this for its own stems below; it is also the EXPOSED build-time hook a
# CONSUMING GAME uses for a custom shader stage (ENG-2.C.3 / Issue 5) — point it at the game's own
# .hlsl and it compiles through the same per-platform toolchain (no runtime compiler, no committed
# bytecode). STEM is the dotted name ("displace.frag", "ripple.frag") → namespace gbcpp::shaders::<ns>
# and the <ns>.h header the renderer/consumer #includes; SRC is the .hlsl path; OUT is the header path;
# HEADERS_VAR (optional) names a list variable in the caller's scope to append OUT to.
function(gbcpp_generate_shader)
    cmake_parse_arguments(_gs "" "STEM;SRC;OUT;HEADERS_VAR" "" ${ARGN})
    string(REGEX MATCH "[^.]+$" _stage "${_gs_STEM}")
    add_custom_command(
        OUTPUT "${_gs_OUT}"
        COMMAND "${CMAKE_COMMAND}"
                -DFORMAT=${GBCPP_SHADER_FORMAT} -DSTAGE=${_stage} -DSTEM=${_gs_STEM}
                "-DSRC=${_gs_SRC}"
                "-DOUT=${_gs_OUT}"
                "-DTMP=${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/tmp"
                ${GBCPP_SHADER_TOOL_ARGS}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_shader.cmake"
        DEPENDS "${_gs_SRC}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_shader.cmake"
        COMMENT "Generating ${GBCPP_SHADER_FORMAT} shader header for ${_gs_STEM}"
        VERBATIM)
    if(_gs_HEADERS_VAR)
        list(APPEND ${_gs_HEADERS_VAR} "${_gs_OUT}")
        set(${_gs_HEADERS_VAR} "${${_gs_HEADERS_VAR}}" PARENT_SCOPE)
    endif()
endfunction()

set(GBCPP_SHADER_STEMS blit.vert blit.frag tile.vert tile.frag sprite.vert sprite.frag
                       postprocess.vert displace.frag)
set(_gbcpp_shader_headers "")
foreach(_stem IN LISTS GBCPP_SHADER_STEMS)
    string(REPLACE "." "_" _ns "${_stem}")
    gbcpp_generate_shader(
        STEM ${_stem}
        SRC  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/src/${_stem}.hlsl"
        OUT  "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/shaders/generated/${_ns}.h"
        HEADERS_VAR _gbcpp_shader_headers)
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
    src/renderer.cpp
    src/image.cpp)
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
# PRIVATE: lodepng backs image.cpp's decode; no public header exposes a lodepng symbol, so it
# stays an implementation detail consumers link transitively but never see.
target_link_libraries(gbcppengine PRIVATE gbcpp-lodepng)
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
        tests/output_test.cpp
        tests/viewport_test.cpp
        tests/shader_format_test.cpp
        tests/draw_state_test.cpp
        tests/tilemap_test.cpp
        tests/palette_test.cpp
        tests/tile_attributes_test.cpp
        tests/sprite_test.cpp
        tests/frame_finish_test.cpp
        tests/postprocess_test.cpp
        tests/custom_stage_test.cpp
        tests/image_test.cpp)
    target_compile_features(gbcppengine-tests PRIVATE cxx_std_20)
    # tests/ on the include path so test-only helpers (manual_clock.h) resolve.
    target_include_directories(gbcppengine-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # image_test.cpp reads the committed PNG fixtures + the shipped demo asset from the source
    # tree (headless, pure CPU).
    target_compile_definitions(gbcppengine-tests PRIVATE
        GBCPP_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        GBCPP_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
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
# The example hosts are the manual runtime checks for the live platform path (open a
# window, composite real draw state, route real input). They are built — so their use of
# the live SdlPlatform/Renderer compiles and links on every CI platform — but never run
# in CI, which has no display. Off for consumers (built only when the engine is top-level).
option(GBCPP_BUILD_EXAMPLES "Build runnable example hosts" ${PROJECT_IS_TOP_LEVEL})
if(GBCPP_BUILD_EXAMPLES)
    # The beach scene: the per-layer screen-space-effect showcase (ENG-2.C.2.b) — a wavy ocean that
    # waves while the sky/sand stay still, beating over a rock split across the ocean's z. Hand-built
    # tiles, no asset files.
    add_executable(gbcpp-beach-demo examples/beach_demo.cpp)
    target_compile_features(gbcpp-beach-demo PRIVATE cxx_std_20)
    target_link_libraries(gbcpp-beach-demo PRIVATE gbcpp::engine)
    gbcpp_set_warnings(gbcpp-beach-demo)

    # The layer transparency showcase (ENG-2.B.3.a): a real indexed PNG uploaded twice (opaque + with
    # an index-hole) over two scrolling layers. Loads examples/assets/demo_tiles.png at runtime, so the
    # assets dir is copied next to the binary after each build (found via SDL_GetBasePath()).
    add_executable(gbcpp-layer-transparency-demo examples/layer_transparency_demo.cpp)
    target_compile_features(gbcpp-layer-transparency-demo PRIVATE cxx_std_20)
    target_link_libraries(gbcpp-layer-transparency-demo PRIVATE gbcpp::engine)
    gbcpp_set_warnings(gbcpp-layer-transparency-demo)
    add_custom_command(TARGET gbcpp-layer-transparency-demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets"
                "$<TARGET_FILE_DIR:gbcpp-layer-transparency-demo>/assets"
        COMMENT "Copying demo assets next to gbcpp-layer-transparency-demo")

    # The minimal getting-started program (docs/guide/getting-started.md): a window + a scrolling
    # hand-built tile background steered by the d-pad. No asset files. Built on every CI platform so
    # the getting-started source stays guaranteed-compiling; never run in CI (no display).
    add_executable(gbcpp-hello-world examples/hello_world.cpp)
    target_compile_features(gbcpp-hello-world PRIVATE cxx_std_20)
    target_link_libraries(gbcpp-hello-world PRIVATE gbcpp::engine)
    gbcpp_set_warnings(gbcpp-hello-world)

    # The custom shader-stage showcase (ENG-2.C.3 / Issue 5): a CONSUMER-authored radial ripple
    # (examples/shaders/ripple.frag.hlsl) registered as a custom shader stage and driven as a
    # first-class screen-space effect, composing with the built-in row-displacement. The ripple HLSL is
    # compiled to this platform's bytecode by the SAME build-time generator the engine uses — the
    # exposed gbcpp_generate_shader hook, pointed at a game's own .hlsl outside shaders/src/. This both
    # demonstrates the consumer build path AND is the CI signal that a consumer-authored fragment
    # generates cleanly on SPIR-V + DXIL + MSL. Reuses the transparency demo's committed PNG.
    set(_gbcpp_ripple_header
        "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders/shaders/generated/ripple_frag.h")
    gbcpp_generate_shader(
        STEM ripple.frag
        SRC  "${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders/ripple.frag.hlsl"
        OUT  "${_gbcpp_ripple_header}")
    add_custom_target(gbcpp-example-shaders DEPENDS "${_gbcpp_ripple_header}")

    add_executable(gbcpp-custom-shader-demo examples/custom_shader_demo.cpp)
    target_compile_features(gbcpp-custom-shader-demo PRIVATE cxx_std_20)
    target_link_libraries(gbcpp-custom-shader-demo PRIVATE gbcpp::engine)
    # The demo #includes the build-generated ripple_frag.h (same include path the engine uses for its
    # own generated shaders); generate it before the demo compiles.
    add_dependencies(gbcpp-custom-shader-demo gbcpp-example-shaders)
    target_include_directories(gbcpp-custom-shader-demo PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated-shaders)
    gbcpp_set_warnings(gbcpp-custom-shader-demo)
    add_custom_command(TARGET gbcpp-custom-shader-demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/assets"
                "$<TARGET_FILE_DIR:gbcpp-custom-shader-demo>/assets"
        COMMENT "Copying demo assets next to gbcpp-custom-shader-demo")
endif()
