#!/usr/bin/env bash
# Build SDL_shadercross once and cache it under a tools dir, idempotently.
#
# Dev-only tooling for shader regeneration — never part of the engine build or the
# shipped binary. Designed to run on a self-hosted Linux x64 runner that already has
# the standard build toolchain (cmake, ninja, a C++ compiler, git, python3); it needs
# no system packages and no sudo. The heavy cost is a one-time DirectXShaderCompiler
# build (pulled as a vendored submodule); subsequent runs reuse the cached binary.
#
# On success, leaves a runnable CLI at "$TOOLS_DIR/bin/shadercross" with its shared
# dependencies in "$TOOLS_DIR/lib" (point LD_LIBRARY_PATH there to run it).
#
# Usage: bootstrap_shadercross.sh <tools-dir> <sdl-source-dir> <shadercross-ref>
set -euo pipefail

TOOLS_DIR="${1:?tools dir required}"
SDL_SRC="${2:?SDL3 source dir required}"
SC_REF="${3:?SDL_shadercross ref required}"

BIN="$TOOLS_DIR/bin/shadercross"
LIB="$TOOLS_DIR/lib"

if [ -x "$BIN" ]; then
    echo "shadercross already cached at $BIN"
    exit 0
fi

echo "::group::Bootstrap SDL_shadercross ($SC_REF)"
mkdir -p "$TOOLS_DIR/bin" "$LIB"
WORK="$TOOLS_DIR/build"
rm -rf "$WORK"
mkdir -p "$WORK"

# ── SDL3 (shadercross links it for the SDL_GPU shader-format enums + logging) ──
# Built from the engine's vendored SDL3 source so the toolchain matches the engine's
# pinned SDL, and installed to a private prefix shadercross's find_package consumes.
SDL_PREFIX="$TOOLS_DIR/sdl3-prefix"
cmake -S "$SDL_SRC" -B "$WORK/sdl3" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF
cmake --build "$WORK/sdl3" --parallel
cmake --install "$WORK/sdl3"

# ── SDL_shadercross (vendored DXC + SPIRV-Cross + SPIRV-Tools/Headers) ──────────
git clone https://github.com/libsdl-org/SDL_shadercross.git "$WORK/src"
git -C "$WORK/src" checkout "$SC_REF"
git -C "$WORK/src" submodule update --init --recursive --depth 1

cmake -S "$WORK/src" -B "$WORK/sc" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$SDL_PREFIX" \
    -DSDLSHADERCROSS_VENDORED=ON \
    -DSDLSHADERCROSS_CLI=ON \
    -DSDLSHADERCROSS_TESTS=OFF \
    -DSDLSHADERCROSS_INSTALL=OFF
cmake --build "$WORK/sc" --parallel

# ── Stage the binary + its shared deps at stable paths ─────────────────────────
SC_BUILT="$(find "$WORK/sc" -name shadercross -type f -perm -u+x | head -n1)"
if [ -z "$SC_BUILT" ]; then
    echo "ERROR: shadercross binary not found under $WORK/sc" >&2
    exit 1
fi
cp "$SC_BUILT" "$BIN"
# Collect every shared lib produced by the shadercross + SDL3 builds (libdxcompiler,
# libdxil, libspirv-cross-c-shared, libSDL3, …) so the CLI runs against this lib dir.
find "$WORK/sc" "$WORK/sdl3" "$SDL_PREFIX" -name '*.so*' -type f -exec cp -n {} "$LIB"/ \; 2>/dev/null || true
echo "::endgroup::"
echo "shadercross built: $BIN"
ls -l "$LIB"
