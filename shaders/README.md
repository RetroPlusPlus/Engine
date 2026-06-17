# Engine shaders

The engine's GPU shaders. Authored once in HLSL; **compiled by every build, on every
platform, to that platform's native format** as an ordinary build step. No bytecode is
committed and nothing is cross-built — a developer cloning the repo builds their own
shaders with their platform's standard tools.

## Layout

```
shaders/
├── src/                       ← HLSL source (the human-authored truth)
│   ├── blit.vert.hlsl         ← fullscreen-triangle blit (viewport → swapchain)
│   ├── blit.frag.hlsl         ← samples the viewport texture
│   ├── tile.vert.hlsl         ← fullscreen-triangle over the viewport (tile layer)
│   ├── tile.frag.hlsl         ← tilemap index → atlas sample (the compositor's tile path)
│   ├── sprite.vert.hlsl       ← instanced per-sprite quad from a sprite storage buffer
│   ├── sprite.frag.hlsl       ← indexed atlas → index-0 discard → palette (the sprite path)
│   ├── postprocess.vert.hlsl  ← shared fullscreen-triangle vertex for every post-process stage
│   ├── displace.frag.hlsl     ← row-displacement built-in effect (wavy water / heat haze)
│   ├── ripple.frag.hlsl       ← radial-ripple built-in effect (a water droplet)
│   └── region_select.frag.hlsl← region gate: confine any effect to a shape (inside ? eff : src)
├── include/
│   └── retropp_effect.hlsli   ← preamble injected into a game's custom fragment (source texture + sampler)
├── gen_shader.cmake           ← build-time generator: HLSL → this platform's bytecode → header
├── gen_effect_fields.cmake    ← reflects each custom shader's cbuffer → ScreenSpaceEffect params + packers
└── README.md                  ← this file
```

The generator is wrapped by a CMake function `retropp_generate_shader(STEM … SRC … OUT … [HEADERS_VAR …])`
(root `CMakeLists.txt`), which the engine calls for its own stems above. Each generated header exposes one
ready-to-use `retropp::shaders::<stem>` `ShaderVariants` constant (the renderer binds it directly).

A consuming game does **not** call the generator directly. It registers a **custom shader stage by path** —
`renderer.registerPostProcessStage("game/shaders/foo.frag.hlsl")` — and the CMake function
`retropp_autocompile_shaders(<target>)` (applied once per consumer target) scans the target's sources for
those `.hlsl` paths, compiles each through the generator, and registers it under the path. The game's
shader declares its **own** parameter cbuffer; a second generator (`gen_effect_fields.cmake`) reflects it
and surfaces those fields on `ScreenSpaceEffect` by name (plus a packer that fills the cbuffer), so the
game sets its shader's params **inline** — no uniform struct. See
[`docs/guide/rendering.md`](../docs/guide/rendering.md) "Custom shader stages"; the engine's
`custom_shader_demo` registers three custom shaders this way.

Generated headers land in the **build tree** (`<build>/generated-shaders/shaders/generated/`),
never in the repo. The renderer includes them by the same `shaders/generated/<stem>.h`
path it always has.

## Per-platform generation

CMake compiles each shader to the one format this platform's `SDL_GPU` backend runs,
at build time, with the platform's native tools:

| Platform | Format | Backend | Tools (build-time only) |
|---|---|---|---|
| macOS | MSL | Metal | `glslang` + `spirv-cross` — `brew install glslang spirv-cross` |
| Linux | SPIR-V | Vulkan | `glslang` — `apt install glslang-tools` |
| Windows | DXIL | Direct3D 12 | `dxc` — ships with the Windows SDK |

The generator is a pure CMake script (`cmake -P`) — no Python or other scripting
runtime; CMake is the one tool every build already requires. A missing shader tool
fails the CMake configure with the install hint. The tools are build-time dependencies
only — the shipped binary embeds the bytecode and needs nothing.

Each generated header exposes the same six symbols regardless of platform — `kSpirv` /
`kDxil` / `kMsl` byte arrays plus a per-format entrypoint constant. The formats not
built on this platform are `nullptr` constants; `shader_format.h`'s `selectShader`
treats null data as absent, and a device never reports a format its backend doesn't
run, so the renderer code is identical on every platform.

Editing a `src/*.hlsl` (or `gen_shader.cmake`) regenerates the affected headers on the
next build automatically — they are ordinary build dependencies.

### MSL binding fidelity

`gen_shader.cmake` passes `--msl-decoration-binding` to spirv-cross so Metal
`[[texture(n)]]` / `[[buffer(n)]]` indices come from the HLSL register numbers —
the same slot order the renderer binds resources in. Without it, spirv-cross
allocates indices in variable-ID order, which can silently swap same-set resources
(observed: `tile.frag`'s atlas and tilemap swapping slots).

## Resource bindings (SDL_GPU convention)

Authored per the `SDL_CreateGPUShader` docs so the per-platform compilers map them
correctly across backends:

- Vertex inputs use `TEXCOORD0`, `TEXCOORD1`, … semantics; system values use `SV_*`.
- A fragment shader's sampled texture + sampler live in register `space2`
  (`register(t0, space2)` / `register(s0, space2)`).
- A fragment shader's read-only **storage** textures continue the `t` register file in
  `space2` *after* the sampled textures (sampled first, then storage).
- A fragment shader's **uniform** buffers live in `space3` (`register(b0, space3)`).

## The tile shaders (ENG-2.B.2.b indexed/palette compositor)

`tile.vert.hlsl` emits the same fullscreen triangle as the blit (it covers the offscreen
viewport target); `tile.frag.hlsl` turns each output pixel into a layer-local pixel, applies
the layer scroll, wraps toroidally into the tilemap, `Load`s + unpacks the cell (tile /
palette-select / flip), flips the within-tile offset, `Load`s the palette **index** from the
**indexed** atlas, resolves the cell's palette-select to a palette-store row, and `Load`s the
final colour from the palette store. The tile path is **all integer `Load` — no sampler**
(the shared nearest sampler is the blit path's now). Its bindings:

| Resource | Register | Kind | Bound via | Notes |
|---|---|---|---|---|
| indexed atlas | `t0, space2` | read-only storage texture | `SDL_BindGPUFragmentStorageTextures` | `R8_UINT`, integer `Load`; one palette index per pixel |
| tilemap cells | `t1, space2` | read-only storage texture | (same call, slot 1) | `R32_UINT`, integer `Load`; `packTileCell` layout |
| palette store | `t2, space2` | read-only storage texture | (same call, slot 2) | `RGBA8`, integer `Load`; row = palette-store row, col = index |
| `TileUniforms` | `b0, space3` | uniform buffer | `SDL_PushGPUFragmentUniformData` | `num_uniform_buffers = 1`; 112 bytes, must match `renderer.cpp`'s struct |

`num_samplers = 0`, `num_storage_textures = 3` for the tile fragment shader. The three storage
textures bind in one `SDL_BindGPUFragmentStorageTextures(pass, 0, {atlas, tilemap, store}, 3)`
call.

The cell layout the shader unpacks — `[tile:16][palette:8][flipX:1][flipY:1][reserved:6]` —
mirrors `retropp::packTileCell` / `unpackTileCell` exactly (the unit-tested reference). There is no
priority bit; depth is layer `z` alone.

`TileUniforms` carries a `uTransparentIndex` (ENG-2.B.3.a — per-source index-hole transparency): when
the layer's atlas declares a transparent colour index (`uploadAtlas(..., transparentIndex)`), the tile
fragment shader `discard`s any pixel whose palette index matches it, so that index becomes a hole the
lower layer shows through. The field reuses a previously-unused `TileUniforms` pad slot — the cbuffer
size is unchanged (112 bytes) — and the default `-1` leaves the `discard` untaken, so a faithful opaque
background renders byte-identically to the pre-B.3.a tile path. The per-layer
palette-set → flat-offset map is the uniform's `uint4 uSetOffsets[4]` (16 slots packed 4 per register),
filled from `retropp::paletteSetOffsets`; a colour is read from the flat palette store at
`offset + index` (wrapped to the store width — palettes are arbitrary size, ENG-2.K). The wrap math (`floorModF`) still mirrors
`retropp::sampleTilemap` exactly; the shader wraps in float with `floor()` because HLSL integer `%`
is undefined for negative operands across backends. The per-layer `ScreenSpaceEffect` is not
consumed here (→ ENG-2.C).

## The sprite shaders (ENG-2.B.2.c.1 instanced sprite path)

`sprite.vert.hlsl` is an **instanced per-sprite quad** — 6 `SV_VertexID` values trace a unit
quad's two triangles, one instance (`SV_InstanceID`) per sprite. There is **no vertex buffer**:
each sprite's record is read from a read-only **storage buffer** (the engine's integer-`Load`
storage idiom). The record already holds the quad in **clip space** — the screen→clip transform
(scroll subtraction, viewport scale, top-left-origin V-flip, matching the blit/tile shaders) is
baked CPU-side in `retropp::makeGpuSprite` — so the vertex stage carries **no uniform buffer**, just
the one storage buffer. `sprite.frag.hlsl` turns the interpolated within-sprite UV into a pixel,
flips it per the sprite's flags, addresses the indexed atlas at the sprite's top-left cell
origin + that pixel (a `w×h` sprite reads a contiguous `w×h` atlas rectangle — a 16×16 sprite
spans a 2×2 cell block), `Load`s the palette **index**, **`discard`s index 0** (OBJ
transparency — the background shows through), and `Load`s the colour from the resolved
palette-store row. All integer `Load`, no sampler.

| Resource | Register | Kind | Bound via | Notes |
|---|---|---|---|---|
| sprite records | `t0, space0` | read-only storage buffer (vertex) | `SDL_BindGPUVertexStorageBuffers` | `StructuredBuffer<GpuSprite>`; one 64-byte record per sprite, indexed by `SV_InstanceID` |
| indexed atlas | `t0, space2` | read-only storage texture (fragment) | `SDL_BindGPUFragmentStorageTextures` | `R32_UINT`, integer `Load`; one palette index per pixel (arbitrary, ENG-2.K) |
| palette store | `t1, space2` | read-only storage texture (fragment) | (same call, slot 1) | `RGBA8`, integer `Load`; FLAT colours wrapped W wide — texel `(flat%W, flat/W)` |
| `SpriteFragUniforms` | `b0, space3` | uniform buffer (fragment) | `SDL_PushGPUFragmentUniformData` | atlas cols + tile px + layer alpha; 16 bytes |

Vertex stage: `num_storage_buffers = 1`, `num_uniform_buffers = 0`. Fragment stage:
`num_samplers = 0`, `num_storage_textures = 2`, `num_uniform_buffers = 1`. The sprite's palette
palette flat offset is resolved **CPU-side** (`retropp::spritePaletteOffset`) into the per-sprite
record, so the fragment shader needs no per-layer set→offset uniform (unlike the tile path); a colour
is read from the flat palette store at `offset + index`. The `GpuSprite` record's `attr` carries
`(tile, paletteOffset, flags, size)` and mirrors `retropp::GpuSprite` / `makeGpuSprite` /
`packSpriteFlags` exactly (the unit-tested reference). `flags` is `flipX | flipY` — there is no
priority bit; depth is layer `z` alone.

> **Why the vertex stage has no uniform buffer.** SDL_GPU's Metal backend places a stage's storage
> buffers at `[[buffer]]` indices *offset past* its uniform buffers (`SDL_gpu_metal.m`), but the
> single-pass HLSL→SPIR-V→MSL generator (`--msl-decoration-binding`) maps both a `t0`-space0 storage
> buffer and a `b0`-space1 uniform buffer to `[[buffer(0)]]` — Metal rejects the duplicate slot and
> shader creation fails. A vertex stage with both a storage buffer and a uniform buffer can't be
> expressed for Metal and Vulkan simultaneously through this toolchain. Baking the screen→clip
> transform into each sprite record leaves the vertex stage with a single buffer, sidestepping the
> collision. A genuinely multi-buffer stage would need a richer MSL remap (or `SDL_shadercross`)
> than the current generator provides.

The tile and sprite pipelines share the offscreen viewport target and the same alpha blend, so
TILES and SPRITES layers interleave correctly by `z` in the single viewport pass — the compositor
binds the tile or sprite pipeline per layer by content kind.
