# Retro++ — Developer Guide

This is the developer/modder-facing guide to the engine's public surface. Read it if you're
building a game on top of the engine, forking it for your own port, or modifying engine
behavior. Each runtime subsystem has its own page under this directory — see the index below.

**New here? Start with [getting-started.md](getting-started.md)** — a complete ~60-line program that
opens a window and draws a scrolling background, explained line by line. Then read
[concepts.md](concepts.md) for the mental model, and [how-to.md](how-to.md) for task recipes. The
per-subsystem pages below are the full reference for each part.

## What the engine is

Retro++ is a native, multiplatform engine for building faithful **8-bit / 16-bit,
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

The whole public API lives in the `retropp` namespace, in headers under `include/retropp/`. Engine
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
| [how-to.md](how-to.md) | Task recipes — scroll, walk-behind, HUD, sprites, fades, PNG loading, atlas slicing, menus, and the retained-vs-rebuilt frame patterns. |

### Subsystem reference

| Page | Covers | Headers |
|---|---|---|
| [build-and-consume.md](build-and-consume.md) | Build modes, CMake targets (`retropp::engine` / `retropp::testkit`), consuming the engine via `add_subdirectory`, dependencies. | `version.h` |
| [run-loop-and-timing.md](run-loop-and-timing.md) | The fixed-step simulation loop, the injectable clock, sim/render decoupling + interpolation (`alpha`, `DoubleBuffer`), and the host-selected timing profile. | `run_loop.h`, `clock.h`, `double_buffer.h`, `timing.h` |
| [input.md](input.md) | The 8-button canonical input surface, per-tick held/edge state, the default key/pad maps, controller-family detection, and the runtime-rebindable bindings. | `input.h`, `input_map.h` |
| [platform-and-windowing.md](platform-and-windowing.md) | The host-OS boundary (`Platform` seam), the production `SdlPlatform` (window + GPU device + event pump + native fullscreen + high-DPI), the windowed-host driver, and the headless `MockPlatform` testing seam. | `platform.h`, `sdl_platform.h`, `windowed_host.h` |
| [rendering.md](rendering.md) | The `Renderer` object, the internal viewport + window-filling blit + nearest/bilinear sampling, the per-frame submission model, and shader-format selection. | `renderer.h`, `viewport.h`, `geometry.h`, `output.h`, `shader_format.h` |
| [draw-state.md](draw-state.md) | The `FrameDrawState` / `DrawLayer` submission envelope: arbitrary Z-sorted layers, scroll/size/alpha, the content variant, per-layer & per-sprite geometric transforms, per-layer tilemap wrap modes, the layer-key collision contract, screen-space effects (frame-level + per-layer, region-confined), and the frame-level colour modifier/blend. | `draw_state.h` |
| [tiles-and-colour.md](tiles-and-colour.md) | The indexed-tile + runtime-palette colour model: indexed atlases, palette upload/store, per-layer palette sets, per-tile/sprite palette-select + flip. | `draw_state.h`, `palette.h` |
| [images-and-transparency.md](images-and-transparency.md) | Loading art from PNG (`loadPng` → index plane + embedded palette), source routing, opt-in per-source index-hole transparency, and atlas asset ingestion (`loadAtlas` / `sliceLayout` → an `AtlasManifest` of carved slots). | `image.h`, `renderer.h` |
| [tilemaps.md](tilemaps.md) | Building a tile layer from images: the map-PNG → `IndexGrid` → `TileCatalog` → `AssembledTilemap` → `TileContent` pipeline, multi-atlas tile layers (one layer mixing several sheets via per-cell `atlasSelect`), and collision-map decode. | `tilemap.h`, `image.h`, `draw_state.h` |
| [animation.md](animation.md) | Frame-based animation over the immediate-mode model: the `Animation` / `AnimationFrame` data model, the pure tick→frame resolver (`playbackAt`), the four `PlaybackMode`s, and the game-owned `AnimationPlayer` cursor (plus multi-clip sheets and palette cycling). | `animation.h` |
| [assets-and-embedding.md](assets-and-embedding.md) | Per-asset delivery policy: bake an asset into the binary (`Embed`) or ship it beside the binary (`LoadFromPath`), chosen in the `loadAtlas` / `loadMapPng` call; the engine-wide + per-type defaults; logical paths + the asset root; the build bakes/copies automatically with no build rule. | `asset_policy.h`, `asset_registry.h`, `engine_config.h` |
| [vm-and-routines.md](vm-and-routines.md) | The runtime VM host: registering a surgically-extracted routine and calling it like a typed C++ function, the developer-declared I/O binding, system selection, and the Game Boy RNG presets. | `vm.h`, `gb.h`, `gb_routines.h` |
| [audio.md](audio.md) | Audio: register on the `AudioLibrary` and cue by handle on an `AudioSystem`, the Music/Sfx tag, the `AudioSink` output (`SdlAudioSink`), running many audio systems at once, and console selection. | `audio_system.h`, `audio_library.h`, `audio.h` |

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
| Per-layer & per-sprite geometric transforms (scale/rotate/skew/perspective) | available | draw-state.md |
| Per-layer tilemap wrap mode (Repeat / Clamp / Blank) | available | draw-state.md / tiles-and-colour.md |
| Image ingestion (PNG) + per-source index-hole transparency | available | images-and-transparency.md |
| Atlas asset ingestion (slice an image → manifest; `Single`/`Tileset`/`SpriteSeries`; all 8 read orders) | available | images-and-transparency.md |
| Tilemap image import (map PNG → `IndexGrid`, id-keyed `TileCatalog` → `AssembledTilemap` → `TileContent`; 16-bit grayscale maps; collision-map decode) | available | tilemaps.md |
| Multi-atlas tile layers (one layer mixes several sheets via per-cell `atlasSelect`; flat atlas store) | available | tilemaps.md / tiles-and-colour.md |
| Frame-based animation (`Animation` + pure tick→frame resolver + `AnimationPlayer` cursor; multi-clip sheets, palette cycling) | available | animation.md |
| Direct-RGBA image sources | deferred (gated on a consumer needing non-indexed art) | images-and-transparency.md (seam noted) |
| Window scaling (N× viewport, clamped to display) + nearest/bilinear sampling + native fullscreen + high-DPI | available | rendering.md / platform-and-windowing.md |
| Frame-level screen-space effects (row displacement, post-process chain) | available | rendering.md / draw-state.md |
| Per-layer screen-space effects (`Layer` isolated / `Below` adjustment-layer scope) | available | draw-state.md |
| Region-confined screen-space effects (confine any effect to a polygon / SDF shape) | available | draw-state.md |
| Custom shader-stage hook (game-registered fragment as a first-class effect) | available | rendering.md / draw-state.md |
| Engine-provided post-process display filters (CRT, scanlines) | planned (author as a custom stage today) | rendering.md |
| VM host (run an extracted routine as a typed function) + Game Boy RNG presets | available | vm-and-routines.md |
| VM host hardware-speed throttle (audio-driver path) | available | audio.md |
| VM host multi-instance (anti-channel-stealing) | planned (seam present) | vm-and-routines.md |
| Audio system (register a sound-driver on the `AudioLibrary`, cue by handle on a system, many systems at once, `SdlAudioSink` output) | available | audio.md |
| Audio packs (register an audio file) + anti-channel-stealing routing | planned | audio.md |
| Settings model, SGB rendering, asset bootstrap, fidelity harness | planned | — |

"Planned" means the surface does not exist in the engine library yet. Where a *type seam* for future
work is already present in shipped headers (e.g. `SpriteContent`, `ScreenSpaceEffect`,
`ColorModifier`, `Blend`), the relevant page documents it as a declared seam and says what it does and
does not do today.
