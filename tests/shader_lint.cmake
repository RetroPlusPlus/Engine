# Shader lint — fail if any engine HLSL declares a StructuredBuffer.
#
# The engine compiles HLSL straight to DXIL with dxc (no SDL_shadercross rewrite), and SDL's D3D12
# backend creates storage buffers with StructureByteStride = 0. AMD's driver uses that stride to index
# a StructuredBuffer, so a dynamically-indexed StructuredBuffer collapses every index to element 0 on
# AMD (only the first record is ever read) — while NVIDIA / WARP / Metal / Vulkan index correctly and
# hide the fault. Use ByteAddressBuffer + Load / Load4 instead: byte addressing does not depend on the
# stride, so it reads correctly on every driver.
#
# This gate catches the whole class at configure / CI time on ANY machine — no AMD hardware needed.
# Run as: cmake -DSHADER_ROOT=<engine>/shaders -P tests/shader_lint.cmake
#
# It matches "StructuredBuffer<" (the declaration form) and skips pure // comment lines, so the
# explanatory comments in the shaders that mention the word do not trip it.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SHADER_ROOT)
    message(FATAL_ERROR "shader_lint.cmake: -DSHADER_ROOT=<path to shaders/> is required")
endif()

file(GLOB _files
    "${SHADER_ROOT}/src/*.hlsl"
    "${SHADER_ROOT}/include/*.hlsli"
    "${SHADER_ROOT}/gen_shader.cmake")

set(_offenders "")
foreach(_f IN LISTS _files)
    file(STRINGS "${_f}" _hits REGEX "StructuredBuffer<")
    foreach(_line IN LISTS _hits)
        string(REGEX MATCH "^[ \t]*//" _iscomment "${_line}")
        if(NOT _iscomment)
            get_filename_component(_name "${_f}" NAME)
            list(APPEND _offenders "${_name}:  ${_line}")
        endif()
    endforeach()
endforeach()

if(_offenders)
    string(REPLACE ";" "\n  " _msg "${_offenders}")
    message(FATAL_ERROR
        "shader lint FAILED — a dynamically-indexed StructuredBuffer is an AMD/D3D12 footgun "
        "(SDL leaves StructureByteStride 0 → the index collapses to element 0 on AMD). "
        "Use ByteAddressBuffer + Load4 instead. Offending declaration(s):\n  ${_msg}")
endif()

message(STATUS "shader lint: no StructuredBuffer declarations — OK")
