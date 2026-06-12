# GBCPP-Engine — Developer Guide

This is the developer/modder-facing guide to the engine's public surface. Read it if you're
building a game on top of the engine, forking it for your own port, or modifying engine
behavior. Each runtime subsystem has its own page under this directory — see the index below.

**New here? Start with [getting-started.md](getting-started.md)** — a complete ~60-line program that
opens a window and draws a scrolling background, explained line by line. Then read
[concepts.md](concepts.md) for the mental model, and [how-to.md](how-to.md) for task recipes. The
per-subsystem pages below are the full reference for each part.

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
when a field or function is *declared but not yet realized* (a forward seam for planned
work), the page says so explicitly rather than implying it works today.

## Index

### Start here

| Page | Covers |
|---|---|
| [getting-started.md](getting-started.md) | A complete minimal program — clone → build → a window with a scrolling, steerable tile background, explained block by block. |
| [concepts.md](concepts.md) | The mental model: how the core objects fit, sim/render decoupling, the "a frame is data" idea, and a glossary. |
| [how-to.md](how-to.md) | Task recipes — scroll, walk-behind, HUD, sprites, fades, PNG loading, menus, and the retained-vs-rebuilt frame patterns. |

### Subsystem reference

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

The guide tracks the engine's shipped surface. Each subsystem's page is written and updated **in the
same change that ships the code** — the page is part of the deliverable, not a later write-up. The
guide always describes what the library actually does today; planned surfaces are called out as
planned, never implied to work.

| Subsystem | Status | Guide page |
|---|---|---|
| Build / consume | available | build-and-consume.md |
| Run loop + interpolation | available | run-loop-and-timing.md |
| Timing profile | available | run-loop-and-timing.md |
| Input surface + generalized buttons/profiles + configurable controls | available | input.md |
| Platform / window / GPU device + `EngineConfig` startup bundle | available | platform-and-windowing.md |
| Internal viewport + scaling/letterbox blit + build-time shaders | available | rendering.md |
| Draw-state envelope + layer-key contract | available | draw-state.md |
| Tile compositor + indexed/palette colour | available | tiles-and-colour.md |
| Sprites | available | draw-state.md / tiles-and-colour.md |
| Frame-level colour modifier/blend + N-layer composition | available | draw-state.md |
| Image ingestion (PNG) + per-source index-hole transparency | available | images-and-transparency.md |
| Direct-RGBA image sources | deferred (gated on a consumer needing non-indexed art) | images-and-transparency.md (seam noted) |
| Output scaling modes + fullscreen + custom shader-stage hook + screen-space effects | planned | rendering.md / draw-state.md (seams noted) |
| SM83 VM (RNG / audio driver) | planned | — |
| Audio chain | planned | — |
| Settings model, SGB rendering, asset bootstrap, fidelity harness | planned | — |

"Planned" means the surface does not exist in the engine library yet. Where a *type seam* for future
work is already present in shipped headers (e.g. `SpriteContent`, `ScreenSpaceEffect`,
`ColorModifier`, `Blend`), the relevant page documents it as a declared seam and says what it does and
does not do today.
