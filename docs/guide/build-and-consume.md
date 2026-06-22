# Build & consume

How to build the engine, what targets it exposes, and how a game attaches it.

## Requirements

- CMake 3.28+
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+)
- Git — SDL3 and SameBoy are submodules, so clone with `--recurse-submodules`
- A shader toolchain (build-time): `glslang` on Linux; `glslang` + `spirv-cross` on macOS; the
  Windows SDK's `dxc` on Windows. See the Shader toolchain note below.

```sh
git clone --recurse-submodules <repo-url>
cd retropp-engine
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Targets

| Target | Alias | Purpose |
|---|---|---|
| `retroppengine` | `retropp::engine` | The shipped engine library. Your game links this. |
| `retropp-testkit` | `retropp::testkit` | Test-tooling library (the ROM-fidelity harness). Link it only into test executables, never into a shipped game binary. |

The engine ships **as source** — there is no precompiled-binary distribution. The whole public
API is in the `retropp` namespace under `include/retropp/`.

## Build modes

The same source supports three configurations:

1. **Engine standalone** — configure the engine as the top-level CMake project (the commands
   above). This builds the engine library plus its own unit tests and the runnable examples
   (`hello_world`, `controller_scrolling`, `beach_demo`, `layer_transparency_demo`, …). This is the
   mode the engine is developed and CI-tested in.
2. **Engine as a subproject** — a consuming game adds the engine with `add_subdirectory(engine)`
   and links `retropp::engine`. The engine's own tests are **off by default** in this mode, so a
   consumer's `ctest` shows only the consumer's tests. A consumer that wants the fidelity-harness
   tooling links `retropp::testkit` into its own test target.
3. **Engine + game as one binary** — the subproject mode above, with the game's executable
   linking `retropp::engine` directly, produces a single self-contained binary per platform (an
   `.app` on macOS, etc.). There is no runtime engine dependency to install separately.

### Build options

| Option | Default | Effect |
|---|---|---|
| `RETROPP_BUILD_TESTS` | ON when the engine is top-level | Build the engine's own unit tests (fetches GoogleTest). |
| `RETROPP_BUILD_EXAMPLES` | ON when the engine is top-level | Build the runnable example hosts. |

Both default off when the engine is a subproject, so a consuming build pulls no test dependency and
compiles none of the engine's examples. Override either explicitly, e.g.
`cmake -S . -B build -DRETROPP_BUILD_TESTS=OFF`.

## Consuming the engine

The intended topology: each consuming game **forks** this repository and attaches the fork as a
git submodule in its own tree (e.g. `your-game/engine/`). The game's CMake references it with
`add_subdirectory(engine)` and links the engine target:

```cmake
add_subdirectory(engine)          # the forked Retro++ submodule
target_link_libraries(your-game PRIVATE retropp::engine)
```

Forking (rather than depending on an upstream tag) is deliberate: a port often needs to grow the
engine's surface as it goes, and a fork keeps those changes first-class in the game's own history
while still allowing upstream merges.

### No per-asset build rules

A game writes no CMake to ship its assets, shaders, or VM routines. The engine scans each
engine-linking target's own sources and acts on what the code declares: an image or chiptune routine
marked `Embed` is baked into the binary, one marked `LoadFromPath` is copied beside it, and every
custom shader registered by path is compiled to the platform's GPU bytecode — automatically, per
target. Adding a *new* asset, routine, or shader registration requires re-running CMake (the scan is
a configure-time read of the source). See [assets-and-embedding.md](assets-and-embedding.md) and
[rendering.md](rendering.md).

## Versioning

`retropp::version()` (declared in `retropp/version.h`) returns the engine's semantic version as a
`std::string_view` — e.g. `"0.1.0-dev"`, where `-dev` marks an unreleased working tree; never empty.
It's an identity stamp a consumer can log or display, and carries no behavior.

## Dependencies

- **[SDL3](https://github.com/libsdl-org/SDL)** — the platform/window/GPU/audio boundary, vendored
  as a submodule at `third_party/sdl/`, built statically from source and linked into the engine.
  zlib license; pulled transitively with `--recurse-submodules`.
- **[SameBoy](https://github.com/LIJI32/SameBoy)** — vendored as a submodule at
  `third_party/sameboy/`, pinned to a tagged release. The reference Game Boy / Game Boy Color core:
  the runtime VM backend runs on it (see [vm-and-routines.md](vm-and-routines.md)), and it is the
  core the fidelity test harness (`retropp::testkit`) wraps. MIT-licensed; pulled transitively with
  `--recurse-submodules`. No `GB_*` symbol reaches a public header — consumers link it transitively
  but never see it.
- **[lodepng](https://github.com/lvandeve/lodepng)** — the PNG decoder for image ingestion (see
  [images-and-transparency.md](images-and-transparency.md)). Vendored as single-file source at
  `third_party/lodepng/` (pinned upstream commit), compiled into the engine as its own
  warning-isolated static target — no submodule, no separate build. zlib/MIT.
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time **only**
  when the engine's own tests are built (`RETROPP_BUILD_TESTS`). Never part of a shipped game binary.

### Shader toolchain (build-time only)

Shaders are authored once in HLSL and **compiled to the running platform's native format at build
time** — `glslang` (+ `spirv-cross` on macOS) for SPIR-V/MSL, the Windows SDK's `dxc` for DXIL (see
[rendering.md](rendering.md) and `shaders/README.md`). These tools are dependencies of *building the
engine*, not of the shipped binary — the build embeds the compiled bytecode and the shipped game
needs nothing. A missing shader tool fails the CMake configure with an install hint
(`brew install glslang spirv-cross` / `apt install glslang-tools` / the Windows SDK's `dxc`).

The shipped binary carries only the engine's own code plus the embedded shader bytecode and the
statically-linked SDL3 / lodepng / SameBoy objects. There is no runtime third-party dependency to
install.

## License

Dual-licensed: **AGPL-3.0** for open use, plus a separate **commercial license**. See
`LICENSING.md` and `LICENSE` at the repository root. Vendored dependencies retain their own
licenses.
