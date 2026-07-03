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

# Optional PREAMBLE injection: a game-authored custom shader declares its OWN parameter cbuffer (at
# b1/space3) + main(); the generator prepends the standard preamble (the source texture + sampler +
# sampleSource() + the engine cbuffer at b0; shaders/include/retropp_effect.hlsli) so the shader
# body just samples through sampleSource() and reads its own params. Engine-internal shaders pass no
# PREAMBLE and compile as-is. `_compile_src` is what actually gets compiled below.
#
# The shader's main() is renamed (identifier only — its parameter/return semantics are legal and ignored
# on a non-entry function) and a generated entry point wraps it: on the Viewport evaluation grid (uSnap,
# the crisp default) the wrapper hands the shader the centre of the viewport cell its fragment falls in
# and records the fragment's true uv for sampleSource()'s displacement quantization — so an unmodified
# custom shader evaluates exactly like the composeScale = 1 rasterization. On the Output grid the wrapper
# passes the true uv through — smooth output-resolution evaluation, wholesale.
set(_compile_src "${SRC}")
if(DEFINED PREAMBLE)
    file(READ "${PREAMBLE}" _preamble_text)
    file(READ "${SRC}" _body_text)
    string(REGEX REPLACE "float4[ \t\r\n]+main[ \t\r\n]*\\(" "float4 retroppCustomMain("
           _body_text "${_body_text}")
    set(_trampoline "
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    retroppTrueUv = uv;
    retroppEvalUv = (uSnap != 0u) ? retroppSnapToCellCenter(uv) : uv;
    return retroppCustomMain(retroppEvalUv);
}
")
    set(_compile_src "${TMP}/${STEM}.wrapped.hlsl")
    file(WRITE "${_compile_src}" "${_preamble_text}\n${_body_text}\n${_trampoline}")
endif()

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
             -o "${_spv}" "${_compile_src}")
    if(FORMAT STREQUAL "spirv")
        file(READ "${_spv}" _hex HEX)
        hex_to_byte_array(kBytes "${_hex}" _array)
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
        hex_to_byte_array(kBytes "${_hex}" _array)
    endif()
elseif(FORMAT STREQUAL "dxil")
    set(_dxil "${TMP}/${STEM}.dxil")
    if(STAGE STREQUAL "vert")
        set(_profile vs_6_0)
    else()
        set(_profile ps_6_0)
    endif()
    run_tool("${DXC}" -T "${_profile}" -E main -Fo "${_dxil}" "${_compile_src}")
    file(READ "${_dxil}" _hex HEX)
    hex_to_byte_array(kBytes "${_hex}" _array)
else()
    message(FATAL_ERROR "gen_shader.cmake: unknown FORMAT '${FORMAT}'")
endif()

# The header exposes ONE ready-to-use symbol: `retropp::shaders::<stem>`, a constexpr
# ShaderVariants the engine and any game register directly — no hand-assembly, no per-
# shader namespace. The platform's built format carries the bytes (in a <stem>_detail
# namespace); the other two slots are absent variants (default ShaderBytecode — null
# data), which selectShader skips, and a device never reports a format its backend
# doesn't run.
set(_real "${NS}_detail::kBytes, sizeof(${NS}_detail::kBytes)")
if(FORMAT STREQUAL "spirv")
    set(_spirv_slot "{${_real}, \"${ENTRY_SPIRV}\"}")
    set(_dxil_slot "{}")
    set(_msl_slot "{}")
elseif(FORMAT STREQUAL "dxil")
    set(_spirv_slot "{}")
    set(_dxil_slot "{${_real}, \"${ENTRY_DXIL}\"}")
    set(_msl_slot "{}")
else()
    set(_spirv_slot "{}")
    set(_dxil_slot "{}")
    set(_msl_slot "{${_real}, \"${ENTRY_MSL}\"}")
endif()

file(WRITE "${OUT}"
    "#pragma once\n"
    "// AUTO-GENERATED at build time by shaders/gen_shader.cmake for this platform's GPU\n"
    "// format (${FORMAT}). DO NOT EDIT and DO NOT COMMIT — it is a build artifact.\n"
    "// Source: ${SRC}\n\n"
    "#include \"retropp/shader_format.h\"\n\n"
    "namespace retropp::shaders {\n\n"
    "namespace ${NS}_detail {\n"
    "${_array}\n"
    "}  // namespace ${NS}_detail\n\n"
    "// The compiled shader in every backend format the engine ships. Engine-internal stages consume this\n"
    "// directly; a game's custom stage is registered BY PATH (renderer.registerPostProcessStage(\"...\"))\n"
    "// and resolved here automatically — see retropp_autocompile_shaders / shader_registry.h.\n"
    "inline constexpr ShaderVariants ${NS}{\n"
    "    ${_spirv_slot},  // spirv (Vulkan)\n"
    "    ${_dxil_slot},  // dxil  (Direct3D 12)\n"
    "    ${_msl_slot}};  // msl   (Metal)\n\n"
    "}  // namespace retropp::shaders\n")

message(STATUS "gen_shader: ${OUT} (${FORMAT})")
