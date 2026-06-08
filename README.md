# GBCPP-Engine

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

Early development. This is the **ENG-0** standup: the build system, target layout,
vendored SameBoy dependency, and standalone CI. No run loop, renderer, VM, or audio
exists yet — those land in subsequent sub-blocks.

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
cd GBCPP-Engine
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Building the engine as the top-level project (above) enables the engine's own tests.
When the engine is consumed via `add_subdirectory`, its tests are off by default; a
consumer that wants the fidelity-harness test tooling links the `gbcpp::testkit`
target.

### Targets

| Target | Alias | Purpose |
|---|---|---|
| `gbcppengine` | `gbcpp::engine` | The shipped engine library; consumers link this. |
| `gbcpp-testkit` | `gbcpp::testkit` | Test-tooling library (ROM-fidelity harness). Linked only into test executables, never into a shipped game binary. |

## Dependencies

- **[SameBoy](https://github.com/LIJI32/SameBoy)** — vendored as a submodule at
  `third_party/sameboy/`, pinned to a tagged release. The reference Game Boy core,
  used both for the runtime VM (a stripped subset) and the full-core fidelity test
  harness. MIT-licensed; pulled transitively with `--recurse-submodules`.
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time
  only when the engine's tests are built.

## License

Dual-licensed: **AGPL-3.0** for open use, plus a separate **commercial license**.
See [`LICENSING.md`](LICENSING.md) and [`LICENSE`](LICENSE). Vendored dependencies
retain their own licenses.
