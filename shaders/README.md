# Engine shaders

The engine's GPU shaders. Authored once in HLSL; shipped as per-backend bytecode the
engine selects at runtime. **The build never compiles a shader** — it `#include`s the
committed headers in `generated/`, so neither CI nor the shipped binary needs a shader
compiler.

## Layout

```
shaders/
├── src/                       ← HLSL source (the human-authored truth)
│   ├── blit.vert.hlsl         ← fullscreen-triangle blit (viewport → swapchain)
│   └── blit.frag.hlsl         ← samples the viewport texture
├── generated/                 ← COMMITTED byte-array headers (the build's input)
│   ├── blit_vert.h            ← kSpirv / kDxil / kMsl + per-format entrypoints
│   └── blit_frag.h
├── wrap_headers.py            ← shadercross blobs → generated/*.h
├── bootstrap_shadercross.sh   ← builds + caches SDL_shadercross on a runner
└── README.md                  ← this file
```

## Why three formats

`SDL_GPU` consumes backend-specific bytecode, not shader source, and reports which
format a device accepts via `SDL_GetGPUShaderFormats`:

| Format | Backend | Platforms |
|---|---|---|
| SPIR-V | Vulkan | Linux x64 / ARM64 |
| DXIL | Direct3D 12 | Windows x64 / ARM64 |
| MSL | Metal | macOS |

Each shader is therefore generated to all three. `shader_format.h`'s `selectShader`
picks the matching variant at device-creation time. SPIR-V and DXIL keep the `main`
entrypoint; the Metal path is renamed by SPIRV-Cross (typically `main0`), so each
generated header records its entrypoint per format.

## Resource bindings (SDL_GPU convention)

Authored per the `SDL_CreateGPUShader` docs so shadercross maps them correctly across
backends:

- Vertex inputs use `TEXCOORD0`, `TEXCOORD1`, … semantics; system values use `SV_*`.
- A fragment shader's sampled texture + sampler live in register `space2`
  (`register(t0, space2)` / `register(s0, space2)`).

## Regenerating (dev-only)

Regeneration runs on a runner that has the toolchain — **not** on a dev laptop and
**not** in the engine build. Microsoft ships no macOS/arm64 DXC, and DXIL can only be
produced by DXC, so generation happens on the Linux x64 self-hosted runner via
`SDL_shadercross` (which also emits SPIR-V and MSL).

1. Change a shader under `src/` and push to a `ci/**` branch (or trigger the
   **Regenerate Shaders** workflow manually). `.github/workflows/regenerate-shaders.yml`
   runs on the Linux x64 runner.
2. `bootstrap_shadercross.sh` builds `SDL_shadercross` once (vendored DXC + SPIRV-Cross)
   and caches the binary under `~/.cache/gbcpp/shader-tools` — a one-time heavy build;
   later runs reuse it. No system packages, no sudo (standard build tools only).
3. The workflow compiles each shader to SPIR-V / DXIL / MSL, runs `wrap_headers.py`,
   and uploads the resulting `generated/` headers as the **shader-headers** artifact.
4. Download the artifact (`gh run download <run-id> -n shader-headers -D shaders/generated`)
   and commit the updated headers. The workflow never commits — the commit gate stays
   with a human.

### Pins

- `SDL_shadercross`: commit `1d8b0556eefb11a77bc9c28249d16f7a3e0459e9` (no tagged
  release exists). It vendors DirectXShaderCompiler + SPIRV-Cross + SPIRV-Tools/Headers
  at the SHAs its submodules point to.
- SDL3: built from the engine's vendored `third_party/sdl` so the toolchain matches the
  engine's pinned SDL.
