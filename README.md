# Retro++

A native, multiplatform engine for faithful Game Boy / Game Boy Color game ports.
The engine provides the generic infrastructure a port needs — fixed-step run loop,
SDL_GPU render pipeline with layered compositing, an embedded SM83 VM for the narrow
set of routines that must run as original hardware code (RNG, audio driver), an audio
chain, a settings model, Super Game Boy rendering, a ROM-fidelity test harness, and
asset bootstrapping — while each consuming game supplies its own logic, data, and
assets.

Out of the box, with no enhancements enabled, the engine reproduces the consuming
game's original behavior faithfully. Enhancements (output scaling, world zoom, audio
packs, display filters) are opt-in and off by default.

## Status

Active development. The engine's core is in place and exercised by its first consumer:

- **Run loop & timing** — fixed-step simulation with sim/render decoupling, frame
  interpolation, and a host-selected timing profile.
- **Platform & input** — SDL3 window + `SDL_GPU` device + event pump, native fullscreen,
  high-DPI, and a rebindable multi-button input surface with controller-family detection.
- **Rendering** — an `SDL_GPU` pipeline with an internal viewport, a window-filling
  integer/letterbox blit (nearest/bilinear), and a layered compositor: arbitrary Z-sorted
  tile and sprite layers, indexed atlases with runtime palettes, per-layer alpha, per-layer
  & per-sprite geometric transforms (scale/rotate/skew/perspective), per-layer tilemap wrap
  modes, PNG image ingestion, frame-level colour modifier/blend, per-layer and frame-level
  screen-space effects, and a game-registered custom shader-stage hook. Shaders are generated
  at build time per platform — no runtime shader compiler, no committed bytecode.
- **VM host** — an embedded SM83 core that runs surgically-extracted routines (authored as
  `.asm` and assembled in-process) as ordinary typed C++ functions, with ready-made Game Boy
  RNG presets. No game ROM is ever loaded.

Planned: the audio chain, a settings model, Super Game Boy rendering, the ROM-fidelity test
harness, and asset bootstrapping.

For the full per-subsystem surface and current status, see the
[developer guide](docs/guide/README.md).

## How it's consumed

Each consuming game forks this repository and attaches the fork as a git submodule
in its own tree (e.g. `<game>/engine/`). The game's build references the engine with
`add_subdirectory(engine)` and links the engine target. The engine ships as source —
there is no precompiled-binary distribution.

The first consumer is a port of Pokémon Crystal, which drives the engine's v1 API
surface.

## Build

Requirements:

- CMake 3.28+
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+)
- Git (the SameBoy dependency is a submodule)

Clone with submodules, then configure and build:

```sh
git clone --recurse-submodules <repo-url>
cd retropp-engine
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Building the engine as the top-level project (above) enables the engine's own tests.
When the engine is consumed via `add_subdirectory`, its tests are off by default; a
consumer that wants the fidelity-harness test tooling links the `retropp::testkit`
target.

### Targets

| Target | Alias | Purpose |
|---|---|---|
| `retroppengine` | `retropp::engine` | The shipped engine library; consumers link this. |
| `retropp-testkit` | `retropp::testkit` | Test-tooling library (ROM-fidelity harness). Linked only into test executables, never into a shipped game binary. |

## Dependencies

- **[SDL3](https://github.com/libsdl-org/SDL)** — vendored as a submodule at
  `third_party/sdl/`, built via `add_subdirectory`. The platform layer (window, GPU
  device, event pump, input) targets `SDL_GPU`. Zlib-licensed; pulled with
  `--recurse-submodules`.
- **[SameBoy](https://github.com/LIJI32/SameBoy)** — vendored as a submodule at
  `third_party/sameboy/`, pinned to v1.0.3. The reference Game Boy / Game Boy Color core;
  its emulation core compiles into the engine to back the runtime VM (the planned
  full-core fidelity test harness reuses the same core). MIT-licensed; pulled with
  `--recurse-submodules`.
- **[lodepng](https://github.com/lvandeve/lodepng)** — vendored in-tree at
  `third_party/lodepng/` (pinned, zlib/MIT), compiled as a small static lib and linked
  privately. Decodes indexed/grayscale PNGs for the image-ingestion path; no symbol
  reaches a public header.
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time
  only when the engine's tests are built.

## License

Dual-licensed: **AGPL-3.0** for open use, plus a separate **commercial license**.
See [`LICENSING.md`](LICENSING.md) and [`LICENSE`](LICENSE). Vendored dependencies
retain their own licenses.
