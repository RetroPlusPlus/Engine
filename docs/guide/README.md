# GBCPP-Engine — Developer Guide

This is the developer/modder-facing guide to the engine's public surface. Read it if you're
building a game on top of the engine, forking it for your own port, or modifying engine
behavior. Each runtime subsystem has its own page under this directory — see the index below.

For the build/consume mechanics start with [build-and-consume.md](build-and-consume.md); for
the per-frame "how do I draw something" path start with [rendering.md](rendering.md).

## What the engine is

GBCPP-Engine is a native, multiplatform engine for building faithful **8-bit / 16-bit,
tile-based retro-style games and ports** — the Game Boy / Game Boy Color / NES / SNES / Genesis /
Master System family idiom, and original games made in that style. It supplies the generic
infrastructure such a game needs — a fixed-step run loop, an input surface, a platform/window/GPU
boundary, and an `SDL_GPU` render pipeline with layered compositing — while each consuming game
supplies its own logic, data, and assets.

**Nothing in the engine is hardwired to the Game Boy.** The viewport, palette, and timing
surfaces all ship presets for the wider console family (`ViewportResolution::Snes`,
`PaletteSize::Genesis`, `TickPeriodNs::Hz60`, …) and accept arbitrary values, so you target an
NES screen, a 16-colour palette, or a custom resolution just as easily. The engine's name and its
Game-Boy-flavoured *defaults* come from its first consumer — a port of Pokémon Crystal (Game Boy
Color) — which is the proven path and the reason the GB presets are the defaults; they are
defaults, not constraints.

The design posture, everywhere: **the engine mirrors the data *model* of the 8-/16-bit era,
not any one console's hardware mechanism.** There are no hardware-register variables, no scanline
interrupts, and no mid-frame register pokes anywhere in the public surface. A frame is computed
*whole* from the game's logical inputs and submitted as data; colour is a palette index plus a
palette selected at render time; timing is a host-selected profile, not a baked constant. Out of
the box, with no enhancements enabled, the engine reproduces a consuming game's original behavior
faithfully; enhancements (output scaling modes, world zoom, audio packs, display filters) are
opt-in and off by default.

The whole public API lives in the `gbcpp` namespace, in headers under `include/gbcpp/`. Engine
enums are `PascalCase` (`Button::Up`, `LayerContentKind::Tiles`).

## How these docs are organized

One page per subsystem. Each page documents the **public surface** (the types and functions you
call), explains the **behavior and intent** behind it, and — where relevant — tells you **where
to change things** if you're modding or forking. The pages are written against the shipped code;
when a field or function is *declared but not yet realized* (a forward seam for a later
sub-block), the page says so explicitly rather than implying it works today.

## Index — shipped subsystems

| Page | Covers | Headers |
|---|---|---|
| [build-and-consume.md](build-and-consume.md) | Build modes, CMake targets (`gbcpp::engine` / `gbcpp::testkit`), consuming the engine via `add_subdirectory`, dependencies. | `version.h` |
| [run-loop-and-timing.md](run-loop-and-timing.md) | The fixed-step simulation loop, the injectable clock, sim/render decoupling + interpolation (`alpha`, `DoubleBuffer`), and the host-selected timing profile. | `run_loop.h`, `clock.h`, `double_buffer.h`, `timing.h` |
| [input.md](input.md) | The 8-button canonical input surface, per-tick held/edge state, the default key/pad maps, controller-family detection, and the runtime-rebindable bindings. | `input.h`, `input_map.h` |
| [platform-and-windowing.md](platform-and-windowing.md) | The host-OS boundary (`Platform` seam), the production `SdlPlatform` (window + GPU device + event pump), the windowed-host driver, and the headless `MockPlatform` testing seam. | `platform.h`, `sdl_platform.h`, `windowed_host.h` |
| [rendering.md](rendering.md) | The `Renderer` object, the internal viewport + integer-scale/letterbox output, the per-frame submission model, and shader-format selection. | `renderer.h`, `viewport.h`, `geometry.h`, `shader_format.h` |
| [draw-state.md](draw-state.md) | The `FrameDrawState` / `DrawLayer` submission envelope: arbitrary Z-sorted layers, scroll/size/alpha, the content variant, the layer-key collision contract, and the effect/modifier/blend seams. | `draw_state.h` |
| [tiles-and-colour.md](tiles-and-colour.md) | The indexed-tile + runtime-palette colour model: indexed atlases, palette upload/store, per-layer palette sets, per-tile/sprite palette-select + flip. | `draw_state.h`, `palette.h` |
| [images-and-transparency.md](images-and-transparency.md) | Loading art from PNG (`loadPng` → index plane + embedded palette), source routing, and opt-in per-source index-hole transparency. | `image.h` |

## Coverage / status

The guide tracks the engine's shipped surface. Subsystems land sub-block by sub-block; each new
sub-block adds or updates its page here **in the same change that ships the code** — the page is
part of the deliverable, not a later write-up.

| Subsystem | Status | Guide page |
|---|---|---|
| Build / consume (ENG-0) | shipped | build-and-consume.md |
| Run loop + interpolation (ENG-1) | shipped | run-loop-and-timing.md |
| Timing profile | shipped | run-loop-and-timing.md |
| Input surface + generalized buttons/profiles (ENG-1 / ENG-2.A) + configurable controls | shipped | input.md |
| Platform / window / GPU device + `EngineConfig` startup bundle (ENG-2.A) | shipped | platform-and-windowing.md |
| Internal viewport + scaling/letterbox blit + build-time shaders (ENG-2.B.1) | shipped | rendering.md |
| Draw-state envelope + layer-key contract (ENG-2.B.2.a) | shipped | draw-state.md |
| Tile compositor + indexed/palette colour (ENG-2.B.2.a/b) | shipped | tiles-and-colour.md |
| Sprites (ENG-2.B.2.c.1) | shipped | draw-state.md / tiles-and-colour.md |
| Frame-level colour modifier/blend + N-layer hardening (ENG-2.B.2.c.2) | shipped | draw-state.md |
| Image ingestion (PNG) + per-source index-hole transparency (ENG-2.B.3.a) | shipped | images-and-transparency.md |
| Direct-RGBA image sources (ENG-2.B.3.b) | deferred (gated on a consumer needing non-indexed art) | images-and-transparency.md (seam noted) |
| Output scaling modes + fullscreen + custom shader-stage hook + screen-space effects (ENG-2.C) | not yet shipped | rendering.md / draw-state.md (seams noted) |
| SM83 VM (RNG / audio driver) | not yet shipped | — |
| Audio chain | not yet shipped | — |
| Settings model, SGB rendering, asset bootstrap, fidelity harness | not yet shipped | — |

"Not yet shipped" means the surface does not exist in the engine library yet. Where a *type
seam* for future work is already present in shipped headers (e.g. `SpriteContent`,
`ScreenSpaceEffect`, `ColorModifier`, `Blend`), the relevant page documents it as a declared
seam and says what it does and does not do today.
