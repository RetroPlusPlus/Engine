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
enums are `PascalCase` (`PadButton::FaceSouth`, `LayerContentKind::Tiles`).

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
| [how-to.md](how-to.md) | Task recipes — scroll, walk-behind, HUD, sprites, fades, recolour, PNG loading, atlas slicing, animation, tweening, menus, and the retained-vs-rebuilt frame patterns. |

### Subsystem reference

| Page | Covers | Headers |
|---|---|---|
| [build-and-consume.md](build-and-consume.md) | Build modes, CMake targets (`retropp::engine` / `retropp::testkit`), consuming the engine via `add_subdirectory`, dependencies. | `version.h` |
| [run-loop-and-timing.md](run-loop-and-timing.md) | The fixed-step simulation loop, the injectable clock, sim/render decoupling + interpolation (`alpha`, `DoubleBuffer`), and the host-selected timing profile. | `run_loop.h`, `clock.h`, `double_buffer.h`, `timing.h` |
| [input.md](input.md) | The action-based input system: game-defined actions bound to any sources in an `ActionMap` value (keyboard, pad buttons positional/printed-letter/family-qualified, mouse, sticks, triggers), per-tick held/edge/value state, player slots, the active-device signal, the raw pointer/analog surface, and controller vibration (the pad's output channel — per-tick declarative motor state, animation-shaped patterns). | `input.h`, `input_actions.h`, `analog_input.h`, `vibration.h` |
| [platform-and-windowing.md](platform-and-windowing.md) | The host-OS boundary (`Platform` seam), the production `SdlPlatform` (window + GPU device + event pump + native fullscreen + high-DPI), the windowed-host driver, the `EngineConfig` startup bundle + `setActive`, and the headless `MockPlatform` testing seam. | `platform.h`, `sdl_platform.h`, `windowed_host.h`, `engine_config.h` |
| [rendering.md](rendering.md) | The `Renderer` object, the internal viewport + window-filling blit + nearest/bilinear sampling, the per-frame submission model, and shader-format selection. | `renderer.h`, `viewport.h`, `geometry.h`, `output.h`, `shader_format.h` |
| [draw-state.md](draw-state.md) | The `FrameDrawState` / `DrawLayer` submission envelope: arbitrary Z-sorted layers, scroll/size/alpha, the content variant, per-layer & per-sprite geometric transforms, per-layer tilemap wrap modes, the layer-key collision contract, screen-space effects (whole-layer + whole-frame, plus shape-confined via a `Region` that owns its effects — polygons or smooth curves, including cubic / Catmull-Rom boundaries evaluated exactly by a baked SDF mask), the built-in effects (`RowDisplacement`, `Ripple`, `ColorFill` — paint a solid colour / drawn line into a region — and `Gleam`, a luminance-keyed diagonal sheen sweep), the per-region `alpha`, the per-row effect data table (`paramTable` — an array input read per-row by a custom effect), the `stencil()` see-through helper, and whole-frame colour (a `ColorFill` region under a blend mode). | `draw_state.h` |
| [sprites.md](sprites.md) | The `Sprite` drawable in full: the required `key` reconciliation identity and keying rules, placement (`x`/`y`, `origin` vs `pivot`, `center(Space)`), the arbitrary-but-8-aligned `size` read + console presets, art (`atlas`/`tile`/`palette`), texture flips + 90° `rotation`, per-sprite `alpha` and `blend`, the geometric `transform`, the `anchors` articulation resolvers (`anchor(k, Space)` / `toLayer`), the effect carrier (`effects` chain + `regions` — whole-silhouette, quad-space, and Below-scope scene lensing), the silhouette shape query (`asShape` borrow / `freeze` snapshot / `approximate` polygon — exact `contains`/`bounds` collision, in `Quad`/`Layer` space), and what interpolates vs snaps. | `draw_state.h`, `geometry.h`, `sprite_shape.h` |
| [anchors-and-articulation.md](anchors-and-articulation.md) | Multi-part figures from plain sprites: anchors (named art-space points a sprite publishes; `anchor(k, Space)` resolver, label-or-index addressing, flips/rotation mirror them with the art), origin & pivot (`Sprite::origin` — the point `x/y` place; `Sprite::pivot` — the transform spin centre; set both to one point for a joint), `Sprite::center(Space)`, attachment by re-feeding a parent's anchor into a child's position each tick, per-sprite `z` (non-unique within-layer stacking, stable ties), and pinning a `Curve` / any point consumer to an anchor. | `draw_state.h` |
| [blend-modes.md](blend-modes.md) | Container blend modes: a `BlendMode` (Normal / Add / Subtract / Multiply / Screen / Half) on `Region` / `DrawLayer` / `FrameDrawState` beside `alpha` — how a container's pixels combine with what they sit on (additive glows, multiply shadows, screen bloom, halved-average translucency); the `applyBlendMode` CPU mirror. | `draw_state.h`, `postprocess.h` |
| [tiles-and-colour.md](tiles-and-colour.md) | The indexed-tile + runtime-palette colour model: indexed atlases, palette upload/store, each tile/sprite naming its own sheet + palette directly (no per-layer set), the `tiles()` helper, + flip. | `draw_state.h`, `palette.h` |
| [images-and-transparency.md](images-and-transparency.md) | Loading art from PNG (`loadPng` → index plane + embedded palette), source routing, opt-in per-source index-hole transparency, and atlas asset ingestion (`loadAtlas` / `sliceLayout` → an `AtlasManifest` of carved slots). | `image.h`, `renderer.h` |
| [tilemaps.md](tilemaps.md) | Building a tile layer from images: the map-PNG → `IndexGrid` → `TileCatalog` → `AssembledTilemap` → `TileContent` pipeline, one layer mixing several sheets (each cell names its sheet directly), and collision-map decode. | `tilemap.h`, `image.h`, `draw_state.h` |
| [animation.md](animation.md) | Frame-based animation over the immediate-mode model: the `Animation` / `AnimationFrame` data model, the pure tick→frame resolver (`playbackAt`), the four `PlaybackMode`s, and the game-owned `AnimationPlayer` cursor (plus multi-clip sheets and palette cycling). | `animation.h` |
| [tween.md](tween.md) | Value animation over the immediate-mode model: the `Tween` / `TweenSegment` data model, the `Easing` curve set, the pure tick→value resolver (`tweenAt` / `valueAt`), and the game-owned `TweenPlayer` cursor (fades, ramps, yoyos, effect/shader-parameter animation). | `tween.h` |
| [curve.md](curve.md) | The curve primitive: a piecewise-Bézier `Curve` with three authoring doors (Bézier handles, Catmull-Rom through-points, Hermite point+tangent), the pure queries — `at` / `tangent` / arc length (`length` / `atDistance` / `tangentAtDistance`) / `signedDistance` — a bakeable `ArcLengthTable` for repeated queries, and how it composes with a tween (shape vs speed). | `curve.h` |
| [path-walker.md](path-walker.md) | Moving along a curve over time: the `PathPacing` driver (constant speed / eased / a `Tween<float>` distance profile), the pure tick→(position + facing) resolver (`walkAt`), and the game-owned `PathWalker` cursor (hold-last facing, quantize-at-the-write, re-pathing). Completes the data→player family beside `Tween`/`TweenPlayer` and `Animation`/`AnimationPlayer`. | `path_walker.h` |
| [sprite-path.md](sprite-path.md) | Driving a whole sprite along a **route**: `SpritePathNode` (a movement spec + pacing + a facing policy + rotation / scale tween tracks + an animation), the `SpritePath` cursor that composes them off one clock and plays a **sequence** of chained-origin legs (node-local clocks, the wait / sentinel idioms, the four sequence playback modes) with an **interrupt stack** on top (departs from the current position, auto-pops on finish, resumes per `ResumePolicy` — `Continue` drifts the route on from where the detour ended (default), `Return` snaps back — plus explicit `popInterrupt`), and the `applyTo(Sprite&)` write (position, transform, frame art, flip — a union envelope over all held content) beside the raw composed sample. The orchestrator over the path walker. | `sprite_path.h` |
| [assets-and-embedding.md](assets-and-embedding.md) | Per-asset delivery policy: bake an asset into the binary (`Embed`) or ship it beside the binary (`LoadFromPath`), chosen in the `loadAtlas` / `loadMapPng` call; the engine-wide + per-type defaults; logical paths + the asset root; the build bakes/copies automatically with no build rule. | `asset_policy.h`, `asset_registry.h`, `engine_config.h` |
| [vm-and-routines.md](vm-and-routines.md) | The runtime VM host: registering a surgically-extracted routine and calling it like a typed C++ function, the developer-declared I/O binding, system selection, and the Game Boy RNG presets. | `vm.h`, `gb.h`, `gb_routines.h` |
| [audio.md](audio.md) | Audio: register on the `AudioLibrary` and cue by handle on an `AudioSystem`, the Music/Sfx/Vocals tag, the `AudioMixer` volume levels, the `AudioSink` output (`SdlAudioSink`), running many audio systems at once, and console selection. | `audio_system.h`, `audio_library.h`, `audio_mixer.h`, `audio.h` |
| [persistence.md](persistence.md) | Durable storage: the `SaveStore` byte-document store — the platform save directory resolved from the application identity (`AppIdentity` on `EngineConfig::identity`), atomic writes (a crash never corrupts the prior document), the absent-vs-corrupt error split, and consumer schema versions with a registered migration chain applied on read. | `save_store.h`, `app_identity.h`, `engine_config.h` |

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
| Action-based input (game-defined actions, multi-source ActionMap, presets, per-family rows, player slots, active-device signal) | available | input.md |
| Pointer & analog input (mouse cursor in viewport coordinates, wheel, gamepad sticks/triggers) | available | input.md |
| Controller vibration (per-tick declarative `gamepad(player).vibration(MotorLevels)`, animation-shaped patterns, per-slot diff to the device) | available | input.md |
| Platform / window / GPU device + `EngineConfig` startup bundle | available | platform-and-windowing.md |
| Internal viewport + scaling/letterbox blit + build-time shaders | available | rendering.md |
| Draw-state envelope + layer-key contract | available | draw-state.md |
| Tile compositor + indexed/palette colour | available | tiles-and-colour.md |
| Sprites (placement, art, `alpha`/`blend`, transform, anchors, the `effects`/`regions` carrier + Below-scope lensing, the silhouette shape query) | available | sprites.md |
| Sprite anchors + origin/pivot placement + per-sprite z-order (published points, mount-on-anchor attachment, within-layer stacking) | available | anchors-and-articulation.md |
| Frame-level whole-frame colour (`ColorFill` region + blend) + N-layer composition | available | draw-state.md |
| Container blend modes (`BlendMode` on `Region` / `DrawLayer` / `FrameDrawState` beside `alpha`; Add / Subtract / Multiply / Screen / Half + Normal) | available | blend-modes.md |
| Per-layer & per-sprite geometric transforms (scale/rotate/skew/perspective) | available | draw-state.md |
| Per-layer tilemap wrap mode (Repeat / Clamp / Blank) | available | draw-state.md / tiles-and-colour.md |
| Image ingestion (PNG) + per-source index-hole transparency | available | images-and-transparency.md |
| Atlas asset ingestion (slice an image → manifest; `Single`/`Tileset`/`SpriteSeries`; all 8 read orders) | available | images-and-transparency.md |
| Tilemap image import (map PNG → `IndexGrid`, id-keyed `TileCatalog` → `AssembledTilemap` → `TileContent`; 16-bit grayscale maps; collision-map decode) | available | tilemaps.md |
| Multi-sheet tile/sprite layers (one layer mixes several sheets — each cell/sprite names its sheet directly; flat atlas store + global region table) | available | tilemaps.md / tiles-and-colour.md |
| Frame-based animation (`Animation` + pure tick→frame resolver + `AnimationPlayer` cursor; multi-clip sheets, palette cycling) | available | animation.md |
| Value animation (`Tween` + `Easing` curve set + pure tick→value resolver + `TweenPlayer` cursor; fades, ramps, yoyos, effect/shader-parameter animation) | available | tween.md |
| Curve primitive (piecewise Bézier; Catmull-Rom / Hermite authoring; `at` / `tangent` / `atDistance` / `tangentAtDistance` / `signedDistance` queries; bakeable `ArcLengthTable`) | available | curve.md |
| Path walking (`PathWalker` cursor + pure `walkAt` resolver + `PathPacing` — constant speed / eased / `Tween<float>` distance profile; position + facing along a curve over time) | available | path-walker.md |
| Sprite paths (`SpritePath` cursor composing movement + rotation / scale tween tracks + an animation + a facing policy off one clock; chained-origin node **sequences** + sequence playback modes + the **interrupt stack** with `ResumePolicy` drift-on-resume (default) or snap-back; `applyTo(Sprite&)` write plus the raw sample) | available | sprite-path.md |
| Direct-RGBA image sources | deferred (gated on a consumer needing non-indexed art) | images-and-transparency.md (seam noted) |
| Window scaling (N× viewport, clamped to display) + nearest/bilinear sampling + native fullscreen + high-DPI | available | rendering.md / platform-and-windowing.md |
| Frame-level screen-space effects (row displacement, post-process chain) | available | rendering.md / draw-state.md |
| Per-layer screen-space effects (`Layer` isolated / `Below` adjustment-layer scope) | available | draw-state.md |
| Region-confined screen-space effects (a `Region` owns a shape — polygon, curve, or SDF — the effects applied inside it, and an `alpha` for their opacity; layers and the frame own a list of them) | available | draw-state.md |
| Built-in effect library (`RowDisplacement` wave, `Ripple` droplet, `ColorFill` solid colour fill / drawn line, `Gleam` diagonal sheen sweep — name the kind, the engine owns the shader) | available | draw-state.md |
| `stencil()` see-through helper (a `Transparency` effect in a region: make a layer transparent inside or outside a shape, feathered — reveal the layers below or the backdrop; each side an optional effect-able region) | available | draw-state.md |
| Custom shader-stage hook (game-registered fragment as a first-class effect) | available | rendering.md / draw-state.md |
| Per-row effect data table (`ScreenSpaceEffect::paramTable` — an array the game fills each frame, read per-row by a custom effect via `paramRow` / `paramRowAtUv`) | available | draw-state.md |
| Engine-provided post-process display filters (CRT, scanlines) | planned (author as a custom stage today) | rendering.md |
| VM host (run an extracted routine as a typed function) + Game Boy RNG presets | available | vm-and-routines.md |
| VM host hardware-speed throttle (audio-driver path) | available | audio.md |
| VM host multi-instance (anti-channel-stealing) | planned (seam present) | vm-and-routines.md |
| Audio system (register a sound-driver on the `AudioLibrary`, cue by handle on a system, many systems at once, `SdlAudioSink` output) | available | audio.md |
| Audio packs (register an audio file) + anti-channel-stealing routing | planned | audio.md |
| Persistence (`SaveStore` — atomic versioned byte documents at the platform save location; migration chain on read) | available | persistence.md |
| Settings model, SGB rendering, asset bootstrap, fidelity harness | planned | — |

"Planned" means the surface does not exist in the engine library yet. Where a *type seam* for future
work is already present in shipped headers (e.g. `SpriteContent`, `ScreenSpaceEffect`), the relevant
page documents it as a declared seam and says what it does and does not do today.
