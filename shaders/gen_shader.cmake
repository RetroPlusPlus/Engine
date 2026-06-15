# Compile one HLSL shader to THIS platform's GPU bytecode and wrap it in a C++ header.
#
# Build-time, per-platform shader generation in pure CMake script mode — the build
# needs no Python or other scripting runtime; CMake is already required. Invoked per
# shader by the add_custom_command in the root CMakeLists.txt:
#
#   cmake -DFORMAT=<msl|spirv|dxil> -DSTAGE=<vert|frag> -DSTEM=<name>
#         -DSRC=<file.hlsl> -DOUT=<header.h> -DTMP=<dir>
#         [-DGLSLANG=<path>] [-DSPIRV_CROSS=<path>] [-DDXC=<path>]
#         -P shaders/gen_shader.cmake
#
# Each platform's build compiles the shader format its SDL_GPU backend runs, using
# that platform's native tools — nothing is cross-built and no bytecode is committed:
#
#     macOS   → MSL    (glslang HLSL→SPIR-V, then spirv-cross SPIR-V→MSL)   [Metal]
#     Linux   → SPIR-V (glslang HLSL→SPIR-V)                                [Vulkan]
#     Windows → DXIL   (dxc HLSL→DXIL)                                      [Direct3D 12]
#
# The emitted header exposes the exact symbol set the renderer already consumes —
# kSpirv / kDxil / kMsl plus the three per-format entrypoint constants. Only this
# platform's format is a real byte array; the other two are nullptr constants, which
# selectShader treats as absent at runtime. The renderer's includes and code are
# untouched: the header lands in the build tree under the same include path
# (shaders/generated/<stem>.h) instead of being committed.

cmake_minimum_required(VERSION 3.28)

foreach(_required FORMAT STAGE STEM SRC OUT TMP)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "gen_shader.cmake: -D${_required}=... is required")
    endif()
endforeach()

if(NOT DEFINED GLSLANG)
    set(GLSLANG glslangValidator)
endif()
if(NOT DEFINED SPIRV_CROSS)
    set(SPIRV_CROSS spirv-cross)
endif()
if(NOT DEFINED DXC)
    set(DXC dxc)
endif()

file(MAKE_DIRECTORY "${TMP}")
string(REPLACE "." "_" NS "${STEM}")

function(run_tool)
    execute_process(COMMAND ${ARGV}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        list(JOIN ARGV " " _cmd)
        message(FATAL_ERROR "gen_shader.cmake: command failed (${_rc}): ${_cmd}\n${_out}\n${_err}")
    endif()
endfunction()

# Render a lowercase hex string as an `inline constexpr unsigned char` array
# definition, 12 bytes per row, into ${out_var}. The 24-dot group is 12 hex byte
# pairs — CMake's regex flavor has no {n} bounded repetition.
function(hex_to_byte_array name hex out_var)
    string(REGEX REPLACE "(........................)" "\\1\n" _rows "${hex}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _rows "${_rows}")
    string(REGEX REPLACE ", \n" ",\n    " _rows "${_rows}")
    string(REGEX REPLACE ", $" "," _rows "${_rows}")
    set(${out_var} "inline constexpr unsigned char ${name}[] = {\n    ${_rows}\n};" PARENT_SCOPE)
endfunction()

# SPIR-V / DXIL keep "main"; spirv-cross renames the MSL entry ("main" is reserved
# in MSL — the SPIRV-Cross default is main0, detected from the emitted source below).
set(ENTRY_SPIRV "main")
set(ENTRY_DXIL "main")
set(ENTRY_MSL "main0")

if(FORMAT STREQUAL "spirv" OR FORMAT STREQUAL "msl")
    set(_spv "${TMP}/${STEM}.spv")
    run_tool("${GLSLANG}" -D -e main -S "${STAGE}" --target-env vulkan1.2
             -o "${_spv}" "${SRC}")
    if(FORMAT STREQUAL "spirv")
        file(READ "${_spv}" _hex HEX)
        hex_to_byte_array(kSpirv "${_hex}" _array)
    else()
        set(_msl "${TMP}/${STEM}.msl")
        # --msl-decoration-binding: assign [[texture(n)]]/[[buffer(n)]] indices from
        # the SPIR-V binding decorations (register index), matching the slot order
        # SDL_GPU binds resources in. Without it spirv-cross allocates indices in
        # variable-ID order, which can silently swap same-set resources (observed:
        # tile.frag uAtlas/uTilemap swapped → atlas bytes fed to the tilemap slot).
        run_tool("${SPIRV_CROSS}" "${_spv}" --msl --msl-version 20000
                 --msl-decoration-binding --output "${_msl}")
        file(READ "${_msl}" _msl_text)
        if(_msl_text MATCHES "(vertex|fragment|kernel)[ \t]+[A-Za-z0-9_]+[ \t]+([A-Za-z0-9_]+)[ \t]*\\(")
            set(ENTRY_MSL "${CMAKE_MATCH_2}")
        endif()
        # MSL is consumed by Metal as a source string → NUL-terminate; the byte
        # length including the terminator is the variant size.
        file(READ "${_msl}" _hex HEX)
        string(APPEND _hex "00")
        hex_to_byte_array(kMsl "${_hex}" _array)
    endif()
elseif(FORMAT STREQUAL "dxil")
    set(_dxil "${TMP}/${STEM}.dxil")
    if(STAGE STREQUAL "vert")
        set(_profile vs_6_0)
    else()
        set(_profile ps_6_0)
    endif()
    run_tool("${DXC}" -T "${_profile}" -E main -Fo "${_dxil}" "${SRC}")
    file(READ "${_dxil}" _hex HEX)
    hex_to_byte_array(kDxil "${_hex}" _array)
else()
    message(FATAL_ERROR "gen_shader.cmake: unknown FORMAT '${FORMAT}'")
endif()

# The header exposes the same six symbols whichever format was built: the renderer
# references all of kSpirv/kDxil/kMsl + entrypoints unconditionally. Formats not
# built on this platform are nullptr constants — selectShader treats null data as
# absent, and a platform's device never reports a format its backend doesn't run.
set(_decls "")
foreach(_pair "spirv;kSpirv" "dxil;kDxil" "msl;kMsl")
    list(GET _pair 0 _fmt)
    list(GET _pair 1 _name)
    if(FORMAT STREQUAL _fmt)
        string(APPEND _decls "${_array}\n\n")
    else()
        string(APPEND _decls "inline constexpr const unsigned char* ${_name} = nullptr;\n\n")
    endif()
endforeach()

file(WRITE "${OUT}"
    "#pragma once\n"
    "// AUTO-GENERATED at build time by shaders/gen_shader.cmake for this platform's GPU\n"
    "// format (${FORMAT}). DO NOT EDIT and DO NOT COMMIT — it is a build artifact.\n"
    "// Source: shaders/src/${STEM}.hlsl\n\n"
    "namespace retropp::shaders::${NS} {\n\n"
    "${_decls}"
    "inline constexpr const char* kSpirvEntrypoint = \"${ENTRY_SPIRV}\";\n"
    "inline constexpr const char* kDxilEntrypoint  = \"${ENTRY_DXIL}\";\n"
    "inline constexpr const char* kMslEntrypoint   = \"${ENTRY_MSL}\";\n\n"
    "}  // namespace retropp::shaders::${NS}\n")

message(STATUS "gen_shader: ${OUT} (${FORMAT})")
