# Reflect every game-authored custom post-process shader's OWN cbuffer and surface its parameters on
# ScreenSpaceEffect (ENG-2.I.b). Build-time, pure CMake script mode (no Python). Invoked once per build
# from the root CMakeLists after the custom-shader set is known:
#
#   cmake -DSHADERS="<abs;hlsl;paths>" -DOUT_FIELDS=<custom_effect_fields.inc>
#         -DOUT_PACKERS=<custom_effect_packers.h> -P shaders/gen_effect_fields.cmake
#
# A custom shader declares its OWN params, e.g.  cbuffer Params : register(b0, space3) { float2 pivot; float blend; }
# This emits TWO generated files, shared by the whole build (so ScreenSpaceEffect has ONE layout in the
# engine library AND every consumer — ODR-safe; the engine is built from source in the consumer's tree):
#
#   OUT_FIELDS  — the UNION of every custom shader's params, one member each, #include'd INSIDE struct
#                 ScreenSpaceEffect. So the game writes the shader's own names inline:
#                   ScreenSpaceEffect{ .kind = Custom, .customShader = h, .pivot = …, .blend = … }
#   OUT_PACKERS — one inline pack_<ns>(const ScreenSpaceEffect&, std::byte*) per shader: reads that
#                 shader's named fields off the effect and writes its cbuffer bytes (HLSL 16-byte register
#                 packing). The generated registry TU registers each by function pointer; the renderer
#                 calls it to fill the cbuffer — renderer.cpp never reads the param fields, only bytes.
#
# Two shaders may share a param NAME only if they give it the same type (one union member); a type
# conflict is a hard error.

cmake_minimum_required(VERSION 3.28)

foreach(_required OUT_FIELDS OUT_PACKERS)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "gen_effect_fields.cmake: -D${_required}=... is required")
    endif()
endforeach()

# HLSL scalar/vector type → (C++ type ; byte size). Unknown types are a hard error (keep the surface
# small + predictable; extend deliberately).
function(_map_type _htype _cpp_var _size_var)
    if(_htype STREQUAL "float")
        set(${_cpp_var} "float" PARENT_SCOPE)
        set(${_size_var} 4 PARENT_SCOPE)
    elseif(_htype STREQUAL "float2")
        set(${_cpp_var} "Vec2" PARENT_SCOPE)
        set(${_size_var} 8 PARENT_SCOPE)
    elseif(_htype STREQUAL "float3")
        set(${_cpp_var} "Vec3" PARENT_SCOPE)
        set(${_size_var} 12 PARENT_SCOPE)
    elseif(_htype STREQUAL "float4")
        set(${_cpp_var} "Vec4" PARENT_SCOPE)
        set(${_size_var} 16 PARENT_SCOPE)
    elseif(_htype STREQUAL "int")
        set(${_cpp_var} "std::int32_t" PARENT_SCOPE)
        set(${_size_var} 4 PARENT_SCOPE)
    elseif(_htype STREQUAL "uint")
        set(${_cpp_var} "std::uint32_t" PARENT_SCOPE)
        set(${_size_var} 4 PARENT_SCOPE)
    else()
        message(FATAL_ERROR "gen_effect_fields.cmake: unsupported cbuffer field type '${_htype}' "
                            "(supported: float, float2, float3, float4, int, uint)")
    endif()
endfunction()

# Names that are already hand-declared BUILT-IN fields of struct ScreenSpaceEffect (draw_state.h). A custom
# shader may reuse one (e.g. a gleam-style shader's `sweep`, which the built-in Gleam effect also declares);
# when it does, the built-in field WINS — it is not re-declared in the union (that would be a duplicate
# member), and the shader's packer below simply reads e.<name> off that built-in field. Keep in sync with
# the hand-written members of ScreenSpaceEffect.
set(_reserved
    "kind;customShader;amplitude;frequency;phase;axis;edge;scope;center;decay;stencil;feather;fill;fillIntensity;sweep;width;gain;slant;saturation;radius;threshold;intensity;paramTable")

# Accumulators: parallel lists of every union member's name + cpp type (deduped), and the packer bodies.
set(_union_names "")
set(_union_types "")
set(_packers "")

foreach(_src ${SHADERS})
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR "gen_effect_fields.cmake: shader does not exist: ${_src}")
    endif()
    get_filename_component(_fname "${_src}" NAME)            # mirror_ghost.frag.hlsl
    string(REGEX REPLACE "\\.hlsl$" "" _stem "${_fname}")    # mirror_ghost.frag
    string(REPLACE "." "_" _ns "${_stem}")                  # mirror_ghost_frag

    file(READ "${_src}" _text)
    # Strip // line comments so they never leak into the field parse.
    string(REGEX REPLACE "//[^\n]*" "" _text "${_text}")
    # Grab the FIRST cbuffer's body. A custom shader declares exactly one param cbuffer (b0, space3).
    if(NOT _text MATCHES "cbuffer[^{]*{([^}]*)}")
        # No cbuffer → a parameterless shader. Its packer writes nothing (0-byte uniform).
        string(APPEND _packers
            "inline std::uint32_t pack_${_ns}(const ScreenSpaceEffect&, std::byte*) { return 0; }\n")
        continue()
    endif()
    set(_body "${CMAKE_MATCH_1}")

    # Walk the fields in declaration order, computing each one's cbuffer offset (HLSL packs into 16-byte
    # registers; a field that would straddle a 16-byte boundary bumps to the next register).
    set(_offset 0)
    set(_pack_lines "")
    string(REPLACE ";" ";;" _body "${_body}")   # keep empty trailing split stable
    string(REGEX MATCHALL "[A-Za-z0-9_]+[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*" _decls "${_body}")
    foreach(_decl ${_decls})
        string(REGEX MATCH "^([A-Za-z0-9_]+)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)$" _ok "${_decl}")
        if(NOT _ok)
            continue()
        endif()
        set(_htype "${CMAKE_MATCH_1}")
        set(_name  "${CMAKE_MATCH_2}")
        _map_type("${_htype}" _cpp _size)

        # 16-byte-register straddle rule.
        math(EXPR _reg_start "${_offset} - (${_offset} % 16)")
        math(EXPR _reg_end   "(${_offset} + ${_size} - 1) - ((${_offset} + ${_size} - 1) % 16)")
        if(NOT _reg_start EQUAL _reg_end)
            math(EXPR _offset "((${_offset} + 15) / 16) * 16")  # bump to next register
        endif()

        # Union dedupe + type check — but a name that is already a BUILT-IN field of ScreenSpaceEffect is
        # NOT added to the union (the built-in field wins; re-declaring it would be a duplicate member). The
        # packer line below still runs, reading e.<name> off that built-in field.
        list(FIND _reserved "${_name}" _ridx)
        if(_ridx EQUAL -1)
            list(FIND _union_names "${_name}" _idx)
            if(_idx EQUAL -1)
                list(APPEND _union_names "${_name}")
                list(APPEND _union_types "${_cpp}")
            else()
                list(GET _union_types ${_idx} _existing)
                if(NOT _existing STREQUAL _cpp)
                    message(FATAL_ERROR "gen_effect_fields.cmake: custom-shader param '${_name}' is declared "
                                        "as both '${_existing}' and '${_cpp}' across shaders — same name must "
                                        "have one type.")
                endif()
            endif()
        endif()

        string(APPEND _pack_lines
            "    std::memcpy(dst + ${_offset}, &e.${_name}, ${_size});\n")
        math(EXPR _offset "${_offset} + ${_size}")
    endforeach()

    # cbuffer total size rounds up to a 16-byte multiple.
    math(EXPR _total "((${_offset} + 15) / 16) * 16")
    if(_total EQUAL 0)
        set(_total 16)
    endif()
    string(APPEND _packers
        "inline std::uint32_t pack_${_ns}(const ScreenSpaceEffect& e, std::byte* dst) {\n"
        "    std::memset(dst, 0, ${_total});\n"
        "${_pack_lines}"
        "    return ${_total};\n"
        "}\n")
endforeach()

# Emit the union fields (#include'd inside struct ScreenSpaceEffect).
set(_fields_text "")
list(LENGTH _union_names _n)
if(_n GREATER 0)
    math(EXPR _last "${_n} - 1")
    foreach(_i RANGE ${_last})
        list(GET _union_names ${_i} _name)
        list(GET _union_types ${_i} _type)
        string(APPEND _fields_text "    ${_type} ${_name}{};\n")
    endforeach()
endif()

file(WRITE "${OUT_FIELDS}"
    "// AUTO-GENERATED at build time by shaders/gen_effect_fields.cmake. DO NOT EDIT, DO NOT COMMIT.\n"
    "// The UNION of every custom post-process shader's own cbuffer params — #include'd INSIDE struct\n"
    "// ScreenSpaceEffect so a Custom effect sets the shader's own names inline (ENG-2.I.b). Empty when no\n"
    "// custom shader is referenced anywhere in the build.\n"
    "${_fields_text}")

file(WRITE "${OUT_PACKERS}"
    "#pragma once\n"
    "// AUTO-GENERATED at build time by shaders/gen_effect_fields.cmake. DO NOT EDIT, DO NOT COMMIT.\n"
    "// One inline packer per custom shader: reads that shader's named fields off a ScreenSpaceEffect and\n"
    "// writes its cbuffer bytes (HLSL 16-byte register packing). The generated registry TU registers each\n"
    "// by function pointer; the renderer calls it to fill the uniform — it never reads the fields itself.\n\n"
    "#include <cstddef>\n"
    "#include <cstdint>\n"
    "#include <cstring>\n\n"
    "#include \"retropp/draw_state.h\"\n\n"
    "namespace retropp::shaders {\n\n"
    "${_packers}"
    "}  // namespace retropp::shaders\n")

message(STATUS "gen_effect_fields: ${OUT_FIELDS} (${_n} union param field(s))")
