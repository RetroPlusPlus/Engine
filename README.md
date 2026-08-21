# Retro++

A native, multiplatform engine for building faithful **8-bit / 16-bit, tile-based retro
games and ports** — the Game Boy / Game Boy Color / NES / SNES / Genesis / Master System
family idiom, and original games made in that style. It supplies the generic infrastructure
such a game needs — a fixed-step run loop, a platform/window/GPU boundary, an `SDL_GPU` render
pipeline with layered compositing, a system-agnostic VM for the narrow set of routines that must
run as original hardware code (RNG, audio driver), an audio chain, persistent storage for saves
and player files, a ROM-fidelity test harness, and asset bootstrapping — while each consuming game
supplies its own logic, data, and assets.

**Nothing in the engine is hardwired to the Game Boy.** The viewport, palette, timing, and input
surfaces ship presets across the wider console family (`ViewportResolution::Snes`,
`PaletteSize::Genesis`, `TickPeriodNs::Hz60`, …) and accept arbitrary values; the VM selects its
core per target system. The Game-Boy-flavoured *defaults* come from the Game Boy Color port the
engine was first grown against — the proven path, and the reason the GB presets are the defaults.
They are defaults, not constraints.

Out of the box, with no enhancements enabled, the engine reproduces the consuming game's original
behavior faithfully. Enhancements (output scaling, world zoom, audio packs, display filters) are
opt-in and off by default.

## Status

Active development. The engine's core is in place and exercised end to end by a real consumer:

- **Run loop & timing** — fixed-step simulation with sim/render decoupling, frame
  interpolation across the ticks a frame actually ran, and a host-selected timing profile.
- **Platform & input** — SDL3 window + `SDL_GPU` device + event pump, native fullscreen,
  high-DPI, and an action-based input surface: a game declares its own actions, binds each to
  any number of sources, and the engine resolves them per controller family.
- **Rendering** — an `SDL_GPU` pipeline with an internal viewport, a window-filling
  integer/letterbox blit (nearest/bilinear), and a layered compositor: arbitrary Z-sorted
  tile and sprite layers, indexed atlases with runtime palettes, per-layer and per-sprite
  alpha, geometric transforms (scale/rotate/skew/perspective), tilemap wrap modes, PNG image
  ingestion, blend modes, frame-level colour modifier/blend, and region-confined effects with
  analytic and mask-based shapes. Shaders are generated at build time per platform — no
  runtime shader compiler, no committed bytecode.
- **Effects** — a built-in screen-space library (ripple, swirl, row displacement, colour fill,
  gleam, saturation, transparency, stencil, glow, bloom) applied uniformly at frame, layer,
  region and sprite scope, plus a game-registered custom shader stage that can join the
  engine's own emission grammar and obtain a blur by declaration rather than by gathering.
- **Motion** — value tweening, curve primitives with arc-length parameterisation, and sprite
  paths with sequencing and interrupt policies.
- **Audio** — a mixed multi-voice chain with per-type levels, chiptune routines and PCM audio
  packs, production on its own thread, and hosting for a game's own resident sound driver as a
  long-lived addressable machine driven by the player's own verbs.
- **VM host** — a system-agnostic VM that runs surgically-extracted original-hardware routines
  (authored as `.asm`, assembled in-process) as ordinary typed C++ functions; the v1 backend is
  an embedded SM83 core (Game Boy / Game Boy Color), and the backend is pluggable per target
  system. A game may also host a cartridge image and name the regions inside it to read and
  write them directly.
- **Persistence** — versioned, atomically-written save documents; a separate store for a
  player's other files; and registration for arbitrary byte assets the engine never interprets.

Planned: the ROM-fidelity test harness, asset bootstrapping, and positional voices.

For the full per-subsystem surface and current status, see the
[developer guide](docs/guide/README.md).

## How it's consumed

Each consuming game forks this repository and attaches the fork as a git submodule
in its own tree (e.g. `<game>/engine/`). The game's build references the engine with
`add_subdirectory(engine)` and links the engine target. The engine ships as source —
there is no precompiled-binary distribution.

The reference consumer is [Kirpich](https://github.com/etroimcasso/Kirpich), a native Game Boy
(DMG) port, which exercises the engine's v1 API surface end to end.

## Build

Requirements:

- CMake 3.28+
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+)
- Git (the SameBoy dependency is a submodule)

Clone with submodules, then configure and build:

```sh
git clone --recurse-submodules git@github.com:RetroPlusPlus/Engine.git
cd Engine
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
