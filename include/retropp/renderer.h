#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/atlas_manifest.h"  // AtlasManifest (loadAtlas's return type; re-exported here)
#include "retropp/asset_policy.h"  // AssetPolicy (loadAtlas's optional embed/load override)
#include "retropp/draw_state.h"
#include "retropp/image.h"        // AssetSlot, ContentKind, ReadOrder, sliceLayout
#include "retropp/interpolation.h"  // Interpolator (the per-id retained mirror)
#include "retropp/literal_path.h"  // LiteralPath (registerPostProcessStage path must be a string literal)
#include "retropp/output.h"
#include "retropp/palette.h"
#include "retropp/shader_format.h"
#include "retropp/shader_registry.h"  // EffectPacker (custom-shader cbuffer packers)
#include "retropp/viewport.h"

namespace retropp {

// The GPU renderer: owns the offscreen internal viewport (an SDL_GPU colour target the game
// draws into), the tile-compositing pipeline + indexed-atlas/tilemap/palette textures that
// fill it from submitted draw state, the blit pipeline + sampler that scale it onto the
// swapchain, and the per-frame submission. It is constructed from a live device + window
// (handed out by SdlPlatform) — drawing is the renderer's job, the platform owns
// window/device/input.
//
// Colour model: the atlas is INDEXED (one palette index per pixel); palettes are a separate
// amortized resource (uploadPalette → a row of a renderer-owned palette store); each tile cell
// selects a palette within its layer's set; the tile shader applies the colour per-pixel. This
// is the faithful GB/C model — colour = index + selected palette at render time — not a
// baked-RGBA atlas and not a hardware palette-RAM poke.
//
// renderFrame() composites the submitted FrameDrawState into the offscreen viewport (z-sorted,
// alpha-blended; indexed TILES layers and SPRITES layers — instanced per-sprite quads with
// colour-index-0 transparency — interleave by z; frame-level colour modifiers fold into the
// final blit) and then blits it integer-scaled + letterboxed onto the swapchain. Atlas index
// data and palette colour data are each uploaded once (amortized); the draw state references
// them by handle and is rebuilt fresh each frame.
//
// Like SdlPlatform, the renderer needs a live GPU device/swapchain — and so a display the
// headless CI runners lack — so it is exercised by the demos and runtime-verified on a dev
// machine, while its device-free math (draw order, tilemap coordinates, shader-format
// selection) is unit-tested.
class Renderer {
public:
    // The settable default viewport — seeded by EngineConfig::setActive() so a bare
    // `Renderer{device, window}` inherits the host's configured resolution instead of having it
    // threaded to every ctor. ViewportResolution lives in viewport.h (already included). Initializes
    // to GameBoyColor (160×144) — the faithful default until setActive() changes it.
    static inline ViewportResolution defaultViewport = ViewportResolution::GameBoyColor;

    // The settable default blit sampling mode — seeded by EngineConfig::setActive() so a bare
    // `Renderer{device, window}` inherits the host's configured sampling instead of the call site having
    // to apply it. Initializes to Nearest (the faithful crisp-pixel default until setActive() changes it).
    // samplingMode() remains the per-renderer runtime override.
    static inline SamplingMode defaultSamplingMode = SamplingMode::Nearest;

    // The settable default for automatic interpolation — seeded by EngineConfig::setActive() from
    // EngineConfig::interpolation so a bare `Renderer{device, window}` inherits the host's choice.
    // Initializes to true (the smooth baseline). automaticInterpolation() is the per-renderer runtime override.
    static inline bool defaultInterpolation = true;

    // The settable default evaluation grid — seeded by EngineConfig::setActive() from
    // EngineConfig::evaluationGrid so a bare `Renderer{device, window}` inherits the host's choice.
    // Initializes to Viewport (crisp — the analytic paths evaluate on the viewport grid, so the upscaled
    // image is pixel-identical to the viewport-resolution rasterization). evaluationGrid() is the
    // per-renderer runtime override. (EvaluationGrid lives in output.h beside SamplingMode.)
    static inline EvaluationGrid defaultEvaluationGrid = EvaluationGrid::Viewport;

    // Creates the offscreen viewport target, the tile + blit pipelines (selecting the
    // bytecode format the device accepts), and a nearest sampler. Throws std::runtime_error
    // on any GPU resource-creation failure. The window must already be claimed for the
    // device (SdlPlatform does this at construction). `viewport` defaults to `defaultViewport`
    // (GameBoyColor until setActive() changes it).
    //
    // `window == nullptr` builds a COMPOSE-ONLY renderer: it creates the device-side compose
    // resources but no blit pipeline and never acquires a swapchain, so it composes + captures the
    // viewport offscreen (captureViewport) but cannot present (renderFrame's blit is skipped). The
    // only thing the windowed path needs the window for is the swapchain-format query the blit
    // pipeline is built against; compose needs only the device. This is the offscreen-capture seam.
    Renderer(SDL_GPUDevice* device, SDL_Window* window, ViewportResolution viewport = defaultViewport);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Upload an INDEXED tile/sprite atlas once (amortized — at load time / tileset swap),
    // returning a handle the draw state references. ONE palette index per pixel, stored as R32_UINT
    // so a pixel can address an arbitrary palette — NOT RGBA; colour comes from a palette at render
    // time. 8-bit and 16-bit source indices are widened into the 32-bit texel; a 32-bit source
    // uploads directly. The renderer owns the GPU texture; the handle stays valid until the renderer
    // is destroyed (no eviction here). Throws std::runtime_error on a GPU failure.
    //
    // `transparent` is this sheet's structural transparent-index set: the palette indices that render
    // as HOLES (discarded, revealing whatever is behind them). BOTH the tile and sprite paths honour
    // it. The default TransparentIndices::None ({}) declares no structural hole → every index draws
    // (subject only to its palette entry's own alpha — material transparency). Game-Boy-style art opts
    // its OBJ hole in with TransparentIndices::GameBoy ({0}); an arbitrary set is
    // TransparentIndices::of({...}).
    AtlasId uploadAtlas(const std::uint8_t*  indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
    AtlasId uploadAtlas(const std::uint16_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
    AtlasId uploadAtlas(const std::uint32_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);

    // A loaded PNG must NOT be pushed straight to uploadAtlas — that bypasses the slicing system. Load
    // an image via loadAtlas() (below), which slices it into an addressable AtlasManifest; uploadAtlas
    // is only for raw index arrays you author yourself. This overload exists solely to make the
    // image → uploadAtlas route an error: it always throws std::logic_error. (Taking a LoadedImage's
    // .indices.data() and calling a raw-pointer overload can't be caught at the type level — that path
    // is, by construction, the deliberate "I'm specifying the bytes myself" escape hatch — but handing
    // a whole LoadedImage to uploadAtlas is the obvious mistake, and it now fails loudly.)
    AtlasId uploadAtlas(const LoadedImage&);

    // Load an indexed PNG, upload it as ONE atlas, and slice it into addressable sub-asset slots —
    // the ergonomic chain over loadPng → uploadAtlas → sliceLayout. Returns an
    // AtlasManifest { atlas, slots } whose `slots[i]` carries the i-th carved asset's top-left atlas
    // cell + dimensions (in `order`, per `kind`). `order` defaults to the western LeftRightThenDown.
    // `count` caps how many assets are carved (0 = the whole grid; a positive count emits only the
    // first `count` in read order — for a sheet with room for more cells than the art uses, so the
    // manifest holds exactly the real assets, not trailing empties; see sliceLayout). `transparent`
    // passes straight through to uploadAtlas (the structural transparent-index set — None = no hole).
    // A degenerate slice yields an empty `slots` (sliceLayout never throws); a load / decode /
    // GPU failure throws std::runtime_error (loadPng's + uploadAtlas's contract).
    //
    // `framesPerAnimation` is recorded on the returned manifest (AtlasManifest::framesPerAnimation) for
    // an AnimationSeries sheet — the grid holds multiple animations, this many frames each, so
    // manifest.animation(g) yields each animation's run. It is consulted ONLY for ContentKind::AnimationSeries
    // (grouping is a manifest concern, not a carve concern — the slot carve is identical for every grid
    // kind); 0 (the default) leaves the manifest ungrouped. `count`, if set, caps the flat carve first;
    // grouping then applies to the capped result.
    //
    // The path is a LITERAL, project-root-relative logical path (a string literal — the build-time scan
    // reads it to bake or copy the asset, so a runtime/computed path is a compile error; load a runtime
    // file with loadAtlasFromMemory(readFile(...)) instead). `policy` selects whether the
    // atlas image is read from disk (LoadFromPath) or decoded from bytes baked into the binary at build
    // time (Embed). nullopt (the default) falls through to loadAtlas's per-type default (LoadFromPath —
    // atlases are the copyright surface). A LoadFromPath asset resolves against the runtime asset root
    // (assetRoot()); an Embed asset the build did not bake falls through to that disk read.
    AtlasManifest loadAtlas(LiteralPath path, AssetDimensions assetSize,
                            ContentKind kind, ReadOrder order = ReadOrder::LeftRightThenDown,
                            int count = 0, TransparentIndices transparent = TransparentIndices::None,
                            int framesPerAnimation = 0, std::optional<AssetPolicy> policy = {});
    // Same, from an in-memory PNG byte span (headless tests, embeddable assets).
    AtlasManifest loadAtlasFromMemory(std::span<const std::uint8_t> bytes, AssetDimensions assetSize,
                                      ContentKind kind, ReadOrder order = ReadOrder::LeftRightThenDown,
                                      int count = 0, TransparentIndices transparent = TransparentIndices::None,
                                      int framesPerAnimation = 0);

    // Upload one palette's colours once (amortized — on change), returning the handle the draw
    // state's palette set references. ARBITRARY entry count — no cap: the colours are
    // appended to a flat, contiguous renderer-owned palette store (the returned PaletteId IS this
    // palette's flat offset into it), and the store texture grows to fit. Valid until the renderer
    // is destroyed (no eviction). Throws std::runtime_error on a GPU failure.
    //
    // The store keeps 16-bit-per-channel colour. The Rgba8 overload widens each entry losslessly
    // (×257, so 255 → 65535); the Rgba16 overload (for a 16-bit colour source) appends direct. Both
    // produce identical output for an 8-bit palette — the shader samples the UNORM store as float4
    // either way.
    PaletteId uploadPalette(std::span<const Rgba8> colors);
    PaletteId uploadPalette(std::span<const Rgba16> colors);

    // Load a colour PNG as a palette: decode it, slice it one-pixel-per-entry in `order`, and upload the
    // entries to the palette store — the colour-store sibling of loadAtlas (which uploads to the atlas
    // store). Returns the PaletteId of the first entry; the entries are contiguous, so a cell/sprite that
    // names this palette selects entry k by index k (no manifest — palette entries don't scatter the way
    // sliced atlas sub-assets do). `order` defaults to the western LeftRightThenDown; `count` caps how
    // many entries are taken (0 = every pixel; past capacity clamps + logs, per readOrderCells). A
    // truecolour (RGBA) PNG is required — an indexed source throws std::runtime_error (slicePaletteImage's
    // contract); a load / decode / GPU failure throws std::runtime_error.
    //
    // The path is a LITERAL, project-root-relative logical path (a string literal — the build-time scan
    // reads it to bake or copy the asset, so a runtime/computed path is a compile error). `policy` selects
    // whether the image is read from disk (LoadFromPath) or decoded from bytes baked into the binary at
    // build time (Embed). nullopt (the default) falls through to loadPaletteImage's per-type default
    // (Embed — a palette image is bespoke build-time colour data, like a map PNG). A LoadFromPath asset
    // resolves against the runtime asset root (assetRoot()); an Embed asset the build did not bake falls
    // through to that disk read.
    //
    // There is no FromMemory sibling: a palette is already buildable directly from colour data via
    // uploadPalette (which takes Rgba8 / Rgba16 spans). For a runtime-supplied palette PNG, compose the
    // public primitives — uploadPalette(slicePaletteImage(loadPngFromMemory(bytes), order, count)).
    PaletteId loadPaletteImage(LiteralPath path,
                               ReadOrder order = ReadOrder::LeftRightThenDown,
                               int count = 0,
                               std::optional<AssetPolicy> policy = {});

    // Bake a closed curve boundary into a signed-distance mask once, returning a handle a region references
    // via ShapePoints::curveMask. A cubic / Catmull-Rom / arbitrary boundary has no closed-form GPU distance;
    // Curve::signedDistance is sampled over the boundary's bounding box (inflated by `padding` px so radius /
    // stroke inflation up to that margin reads a valid distance) into an R16_FLOAT field, uploaded as a
    // bilinear-sampled texture. `maxResolution` caps the field's longer axis (the shorter scales by aspect).
    // Bake once at setup; a region samples it every frame, and the region transform moves / scales / skews it
    // with no re-bake. The renderer owns the texture; the handle stays valid until the renderer is destroyed
    // (no eviction). Linear + quadratic boundaries do not need a mask — they are exact analytically. Throws
    // std::runtime_error on a GPU failure.
    CurveMaskId bakeCurveMask(const Curve& boundary, float padding = 8.0f, int maxResolution = 256);

    // The one-call ergonomic over bakeCurveMask: bake `boundary`'s mask and return a ready region shape whose
    // boundary IS the curve and whose curveMask is the baked handle (`radius` inflates it, `t` warps it). The
    // default authoring path for a cubic / arbitrary curved region — equivalent to
    // ShapePoints::fromCurve(boundary, radius, t) with .curveMask set from bakeCurveMask(boundary, ...).
    [[nodiscard]] ShapePoints bakeCurveRegion(const Curve& boundary, float radius = 0.0f, Transform t = {},
                                              float padding = 8.0f, int maxResolution = 256);

    // Register a game-authored custom shader stage BY PATH, returning a handle the draw state
    // references via ScreenSpaceEffect{ .kind = Custom, .customShader = <handle> } at EITHER scope — per-
    // layer (DrawLayer::effect) or frame-level (postEffects). A custom shader is a first-class effect kind,
    // composing with the built-ins in the same machinery and driven by the SAME inline parameter fields:
    //
    //   auto stage = renderer.registerPostProcessStage("game/shaders/my_effect.frag.hlsl");
    //
    // That path is the whole registration — no ShaderVariants, no generated-header include, no CMake rule,
    // no uniform struct or size. A build-time source scan (CMake retropp_autocompile_shaders) sees the
    // `.hlsl` path referenced in the code, INJECTS the standard preamble (the source texture + sampler +
    // sampleSource() + the engine edge-mode cbuffer at b0; shaders/include/retropp_effect.hlsli), compiles
    // it to this platform's GPU bytecode, embeds it, REFLECTS the shader's OWN parameter cbuffer (its own
    // named fields at b1/space3) into a generated packer, and registers it under that exact path string.
    // The game's shader is therefore its OWN cbuffer + a `main()` body that samples through sampleSource();
    // the game sets the shader's OWN reflected params as inline fields on the effect (.kind = Custom,
    // .customShader = <handle>, .<param> = …), exactly like a built-in. Two pipelines are built once
    // (no-blend replace for frame-level / Below, premultiplied-over for Layer); handles stay valid until
    // the renderer is destroyed.
    //
    // The path is a LiteralPath: it MUST be a string literal, because the build-time scan reads it out of
    // the source verbatim. Passing a runtime variable / std::string / computed path is a COMPILE error
    // (see literal_path.h) — not a runtime surprise. The runtime std::runtime_error below is the residual
    // backstop for a literal referenced from a source the scan does not read (e.g. a header): no shader
    // was compiled for it. The ShaderVariants overload is the lower-level seam the path form resolves to.
    PostProcessStageId registerPostProcessStage(LiteralPath shaderPath);
    PostProcessStageId registerPostProcessStage(const ShaderVariants& fragment);

    // One frame: composite the submitted INDEXED TILES + SPRITES layers into the offscreen
    // viewport (z-sorted, alpha-blended; per-tile palette-select + flip applied in-shader from
    // the layer's palette set), run the frame-level post-process chain (frame.postEffects; an
    // empty chain is a no-op), then blit the result integer-scaled + letterboxed to the swapchain
    // with the frame-level colour transform. Throws std::invalid_argument on a layer-key collision
    // when the collision policy is Throw (the default in debug builds; see setLayerCollisionPolicy).
    //
    // When interpolation is on (the default), the renderer reads the
    // run loop's sub-tick factor + tick signal from the frame-timing channel (frame_timing.h), reconciles
    // this submission into its per-id retained mirror once per tick, and composites each layer/sprite eased
    // between its previous and current tick state — so the game submits its latest state and the engine
    // blends. With interpolation off (automaticInterpolation(false)) the submission composites verbatim.
    void renderFrame(const FrameDrawState& frame);

    // Automatic interpolation, runtime-dynamic. A renderer starts at defaultInterpolation (seeded from
    // EngineConfig::interpolation by setActive()); this toggles it on the fly. On: ease each object between
    // its previous and current sim-tick state by the loop's sub-tick factor (the per-id mirror tracks the
    // last two ticks). Off: composite each submission exactly as given (no mirror, no blend).
    void               automaticInterpolation(bool enabled) noexcept { interpolation_ = enabled; }
    [[nodiscard]] bool automaticInterpolation() const noexcept { return interpolation_; }

    // Evaluation grid, runtime-dynamic. A renderer starts at defaultEvaluationGrid (seeded from
    // EngineConfig::evaluationGrid by setActive()); this is the runtime override — call it to switch the
    // analytic paths' evaluation granularity on the fly (e.g. a settings toggle). Viewport = the analytic
    // math (transformed tiles, effect regions, displace/ripple) evaluates on the viewport grid, so the
    // upscaled image is pixel-identical to the viewport-resolution rasterization (crisp); Output = evaluate
    // per output pixel (smooth edges/displacement under upscale). A mathematical no-op when the compositor
    // runs at viewport resolution — the choice only bites once placement composites onto a finer grid.
    void                 evaluationGrid(EvaluationGrid grid) noexcept { evaluationGrid_ = grid; }
    [[nodiscard]] EvaluationGrid evaluationGrid() const noexcept { return evaluationGrid_; }

    // Compose `frame` and download the finished viewport image as packed Rgba8 (viewport width × height,
    // row-major, top-to-bottom). Runs the same compose path renderFrame blits — copy pass, layer
    // composite, post-process chain — then downloads the composed offscreen image instead of presenting
    // it (the swapchain blit is skipped; the composed viewport is the captured subject). Blocks on a fence
    // until the download lands. The offscreen intermediates are R16G16B16A16_FLOAT (a colour channel may
    // exceed 1); the download quantizes each channel with round(clamp(v,0,1)·255) — the same 8-bit clamp
    // the swapchain blit applies — so the result matches what a present would show. This is the
    // offscreen-capture seam for the golden-readback harness, not part of the runtime render loop. Works
    // on any renderer (windowed or compose-only).
    [[nodiscard]] std::vector<Rgba8> captureViewport(const FrameDrawState& frame);

    // Compose `frame` at an explicit compose scale and download the finished image (composeScale·viewport
    // wide × tall, packed Rgba8, row-major top-to-bottom). captureViewport(frame) is exactly this at scale 1
    // — the byte-identical golden-capture path. A scale > 1 composes on the finer grid the interpolation path
    // uses, so it captures the output-resolution image the current evaluation grid produces (Viewport →
    // pixel-identical to the scale-1 capture nearest-upscaled; Output → the smooth output-res evaluation).
    // The parity seam for the crisp harness — not part of the runtime render loop; works on any renderer.
    [[nodiscard]] std::vector<Rgba8> captureViewport(const FrameDrawState& frame, int composeScale);

    // The engine renderer. A program constructs exactly one, and Sprite::asShape / freeze / approximate
    // resolve a sprite's AtlasId against its already-uploaded atlas pixels through this handle. Throws
    // std::logic_error if called before the renderer is constructed.
    [[nodiscard]] static const Renderer& instance();

    // The uploaded pixel size of an atlas, or {0, 0} for an unknown id. Reads the CPU atlas mirror the
    // upload already keeps; the sprite shape query's cell math uses it.
    [[nodiscard]] PixelSize atlasPixelSize(AtlasId atlas) const noexcept;

    // Is the atlas pixel at (x, y) VISIBLE — its palette index is not a structural hole for that sheet? An
    // out-of-bounds coordinate or an unknown atlas is not visible. This is the device-free coverage read the
    // sprite shape query answers contains() with, off the same CPU atlas mirror the upload keeps — no GPU.
    [[nodiscard]] bool atlasVisibleAt(AtlasId atlas, int x, int y) const noexcept;

    // The runtime reaction when a frame submits colliding layer keys (duplicate z or label).
    // Defaults to kDefaultCollisionPolicy (Throw in debug, WarnAndResolve in release); a host
    // can override it (e.g. force Throw in a soak test, or WarnAndResolve in a kiosk build).
    void setLayerCollisionPolicy(LayerKeyCollisionPolicy policy) noexcept { collisionPolicy_ = policy; }
    [[nodiscard]] LayerKeyCollisionPolicy layerCollisionPolicy() const noexcept { return collisionPolicy_; }

    // Blit sampling, runtime-dynamic. A renderer starts at defaultSamplingMode (seeded from
    // config.enhancements.sampling by EngineConfig::setActive(), so the call site doesn't apply it); this
    // is the runtime override — call it to switch sampling on the fly (e.g. a settings toggle).
    // Nearest = crisp integer pixels (the faithful default); Bilinear = smoothed upscale. Both samplers are
    // created at construction; the blit binds the one this selects. The viewport always fills the window at
    // the largest integer scale that fits (integerScaleToFitRect) — output SIZE is the window's size, owned
    // by the platform (Platform::setWindowSize), not a renderer mode.
    void samplingMode(SamplingMode mode) noexcept { sampling_ = mode; }
    [[nodiscard]] SamplingMode samplingMode() const noexcept { return sampling_; }

private:
    // An uploaded indexed atlas: not its own texture — every atlas lives as a region of the single
    // flat atlas store (atlasStore_), exactly as every palette lives in the flat palette store.
    // `data` is the CPU mirror of this atlas's R32_UINT pixels (kept so the store can be recreated +
    // re-uploaded whole when a new atlas grows it, mirroring paletteData_); `width`/`height` are its
    // pixel dimensions (tile-grid addressing); `transparent` its structural transparent-index set
    // ({} = none); `storeY` its top row in the vertically-stacked store. AtlasId = index.
    struct AtlasEntry {
        std::vector<std::uint32_t> data;
        int                width = 0, height = 0, storeY = 0;
        TransparentIndices transparent{};
    };
    // A baked curve signed-distance mask: its own R16_FLOAT texture (sampled, bilinear) plus the shape-local
    // bake box the shader maps fragments into. CurveMaskId is 1-based (0 = none); curveMasks_[id − 1] is this.
    struct CurveMaskEntry {
        SDL_GPUTexture* texture = nullptr;
        Vec2            bakeMin{};      // bake box min corner (shape-local px)
        Vec2            bakeExtent{};   // bake box size (shape-local px)
        int             width = 0, height = 0;
    };
    // A per-layer tilemap cell texture (R32_UINT, packTileCell'd), recreated when its tile
    // dimensions change.
    struct TilemapTex { SDL_GPUTexture* texture = nullptr; int widthInTiles = 0; int heightInTiles = 0; };
    // A per-layer sprite storage buffer (GpuSprite records). Grow-only: recreated only when a
    // frame's sprite count exceeds its capacity (in sprites), reused across frames otherwise.
    // `count` is the number of GpuSprite records actually uploaded (the art-drawing sprites — a Below-scope
    // lens draws no art, so it is excluded), which is the instance count drawLayer issues; `capacity` is the
    // buffer's allocated record capacity (grow-only).
    struct SpriteBuf { SDL_GPUBuffer* buffer = nullptr; int capacity = 0; int count = 0; };

    // Core indexed-atlas upload (R32_UINT); the public uploadAtlas overloads widen into it.
    AtlasId uploadAtlas32(const std::uint32_t* indices, int width, int height, TransparentIndices transparent);

    // Recreate atlasStore_ from the atlases_ CPU mirrors: stack them vertically (width = widest,
    // height = Σ heights), assign each entry's storeY, upload the whole store. Called after
    // every uploadAtlas — uploads are amortized (load time), exactly like the palette store.
    void rebuildAtlasStore();

    // Recreate paletteStore_ from the flat paletteData_ mirror: a kPaletteStoreWidth-wide
    // R16G16B16A16_UNORM texture grown in height to hold every entry, the whole store re-uploaded.
    // Called by both uploadPalette overloads after they append — uploads are amortized (load time).
    void rebuildPaletteStore();

    // Compose the finished viewport image for `frame` into an offscreen target and return which texture
    // holds it (target_ when the post-process chain is empty, else the chain's final ping-pong scratch).
    // This is everything renderFrame does up to the blit: the copy pass (tilemap/sprite/row-data uploads,
    // recording transfer buffers in `scratch` for the caller to release after submit), the layer
    // composite, and the post-process chain. renderFrame = composeViewport + blit; captureViewport =
    // composeViewport + download. The composed bytes are identical in both — one compose path, no drift.
    // When `interpolate` is set, each layer's / sprite's placement position is read as the eased float
    // (interpolatedLayerScroll / interpolatedSpritePos) at `alpha` — the sub-pixel position on the
    // output-resolution grid; the frame's integer fields are the fallback for an id with no history.
    // Off (captureViewport, interpolation disabled) places at the frame's integer positions.
    SDL_GPUTexture* composeViewport(SDL_GPUCommandBuffer* cmd, const FrameDrawState& frame,
                                    std::vector<SDL_GPUTransferBuffer*>& scratch,
                                    float alpha, bool interpolate);

    // The compose scale for this frame: the window drawable's integer-scale-to-fit factor (clamped to
    // [1, kMaxComposeScale]) when interpolation is on and a window exists; 1 otherwise. 1 is the
    // faithful path (compose at viewport res, blit upscales) — the byte-identical guarantee attaches to
    // it. captureViewport does not call this (it pins 1 directly).
    [[nodiscard]] int resolveComposeScale() const;

    // Point composeScale_/composeW_/composeH_ at `scale` and (re)create the four offscreen targets
    // (target_/post0_/post1_/layerScratch_) at the new compose grid. A no-op when `scale` already
    // matches and the targets exist — guarded so a steady window never reallocates. Called from the
    // ctor (scale 1), from renderFrame after resolveComposeScale, and from captureViewport (pinning 1).
    void resizeComposeTargets(int scale);

    void releaseAtlases();
    void releaseTilemaps();
    void releaseSpriteBuffers();
    void releaseCustomStages();
    void releaseBatchResources();

    // Build the instanced-additive region pipeline for a custom stage whose shader carries the
    // `// @retropp:additive` declaration: the engine's region_batch vertex stage + the shader's BATCHED
    // fragment variant, with ADDITIVE colour blend (ONE / ONE) and destination-alpha-preserving alpha
    // blend (ZERO / ONE) — so many same-shader additive regions accumulate in ONE pass. Called by the
    // path-registering overload when findBatchedShaderVariants(path) is non-null; returns nullptr on
    // failure (the stage then stays on the per-region path).
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildBatchedStagePipeline(const ShaderVariants& batchedFragment);

    // Build ONE gathered-region pipeline for a custom stage whose shader has a gather variant: the
    // shared fullscreen-triangle vertex stage + the shader's GATHER fragment variant (1 sampler + 1 storage
    // texture + 2 uniforms + 1 fragment storage buffer of per-region records). `blend` picks the replace
    // pipeline (no blend, frame-level / Below / mid-chain) or the premultiplied-over pipeline (ONE /
    // ONE_MINUS_SRC_ALPHA — the Normal-layer last-step composite onto target_). Returns nullptr on failure
    // (the stage then stays on the per-region path). Registered as the pair customGather_/customGatherBlend_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildGatherStagePipeline(const ShaderVariants& gatherFragment,
                                                                    bool blend);

    // Build the sprite-inline pipeline for a custom stage whose shader has a sprite variant: the engine's
    // sprite vertex stage + the shader's SPRITE fragment variant (the sprite fragment with the custom body
    // injected), with the SAME resource contract and alpha blend as the stock sprite pipeline. Called by the
    // path-registering overload when findSpriteShaderVariants(path) is non-null; returns nullptr on failure
    // (the stage then can't run inline on a sprite). Registered as customSprite_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildSpriteStagePipeline(const ShaderVariants& spriteFragment);

    // Build the below-custom pipeline for a custom stage whose shader has a sprite-below variant: the engine's
    // sprite vertex stage + the shader's SPRITE_BELOW fragment variant (the below sprite fragment with the
    // custom body injected, sampleSource reading the scene), with the SAME resource contract and blend as the
    // built-in spriteBelow_ pipeline. Called by the path-registering overload when
    // findSpriteBelowShaderVariants(path) is non-null; the built-in spriteBelow_ is built from this too (the
    // stock fragment). Registered as customSpriteBelow_.
    [[nodiscard]] SDL_GPUGraphicsPipeline* buildSpriteBelowStagePipeline(const ShaderVariants& belowFragment);

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportResolution       viewport_;
    // The compose grid content is rasterized onto: viewport × composeScale_. Held distinct from the
    // viewport's normalization role — effects and regions measure against the viewport (invViewportW/H,
    // region radii), while offscreen-target sizing and content placement use the compose grid. At
    // composeScale_ == 1 the two coincide. Resolved per frame from the window drawable size when
    // interpolation is on (so sub-pixel motion has a finer grid to land on), pinned to 1 otherwise
    // (interpolation off, no window / headless, or captureViewport — the faithful, byte-identical path).
    // resolveComposeScale() computes it; resizeComposeTargets() reallocates the offscreen targets when
    // it changes. composeW_/composeH_ are viewport × composeScale_.
    int                      composeScale_ = 1;
    int                      composeW_     = 0;   // viewport_.width  * composeScale_
    int                      composeH_     = 0;   // viewport_.height * composeScale_
    // The compose grid's ceiling. Sub-pixel placement is already visually continuous well below this;
    // the cap bounds the four float16 offscreen targets' memory (viewport×N)² · 8 B · 4 — at 16× and a
    // 160×144 viewport that is 2560×2304 ≈ 189 MB, reached only by a >4K drawable. Below the cap the
    // compose scale equals the window's integer-scale-to-fit factor, so the blit is a 1:1 centring copy
    // (fill parity with the faithful path); above it the blit integer-upscales the remainder as usual.
    static constexpr int     kMaxComposeScale = 16;
    // macOS/Metal ONLY: SDL's Metal GPU backend implements the BLOCKING swapchain wait as a CPU
    // busy-wait spin (SDL_gpu_metal.m METAL_WaitForFences: `while(!complete) // Spin!`), so the
    // blocking acquire burns a core while VSYNC holds the fence to vblank. On Metal we use the
    // NON-blocking acquire (a single fence check, skip-if-not-ready) and let the host-loop frame
    // deadline pace cadence instead. Vulkan/D3D12 OS-block correctly, so they keep the blocking
    // acquire. Set once at ctor.
    bool                     acquireNonBlocking_ = false;
    SDL_GPUTexture*          target_       = nullptr;  // offscreen viewport colour target
    SDL_GPUTexture*          post0_        = nullptr;  // post-process scratch A (viewport-sized)
    SDL_GPUTexture*          post1_        = nullptr;  // post-process scratch B (ping-ponged with A)
    SDL_GPUTexture*          layerScratch_ = nullptr;  // per-layer effect scratch; swapped with target_ for Below
    SDL_GPUGraphicsPipeline* tile_         = nullptr;  // indexed tilemap → atlas → palette compositor
    SDL_GPUGraphicsPipeline* sprite_       = nullptr;  // instanced per-sprite-quad → atlas → palette
    SDL_GPUGraphicsPipeline* spriteBelow_  = nullptr;  // Below-scope sprites: scene-reading, coverage-masked
    SDL_GPUGraphicsPipeline* displace_     = nullptr;  // row-displacement post-process stage
    SDL_GPUGraphicsPipeline* displaceBlend_ = nullptr; // displace + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* ripple_       = nullptr;  // built-in radial ripple post-process stage
    SDL_GPUGraphicsPipeline* rippleBlend_  = nullptr;  // ripple + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* colorFill_      = nullptr; // built-in colour fill / tint post-process stage
    SDL_GPUGraphicsPipeline* colorFillBlend_ = nullptr; // colorFill + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* colorFillGather_      = nullptr; // N ColorFill regions in ONE pass, replace
    SDL_GPUGraphicsPipeline* colorFillGatherBlend_ = nullptr; // ColorFill gather + premultiplied-over composite
    SDL_GPUGraphicsPipeline* gleam_          = nullptr; // built-in gleam (diagonal sheen sweep) post-process stage
    SDL_GPUGraphicsPipeline* gleamBlend_     = nullptr; // gleam + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* saturation_      = nullptr; // built-in colour-saturation (desaturate toward luma) post-process stage
    SDL_GPUGraphicsPipeline* saturationBlend_ = nullptr; // saturation + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* regionSelect_      = nullptr; // region gate: inside?eff:src, replace
    SDL_GPUGraphicsPipeline* regionSelectBlend_ = nullptr; // region gate + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* regionSelectCurve_      = nullptr; // curve-boundary region gate (analytic), replace
    SDL_GPUGraphicsPipeline* regionSelectCurveBlend_ = nullptr; // curve-boundary region gate + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionStencil_      = nullptr; // region see-through (source × survival), replace
    SDL_GPUGraphicsPipeline* regionStencilBlend_ = nullptr; // region see-through + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* regionStencilCurve_      = nullptr; // curve-boundary region see-through (analytic), replace
    SDL_GPUGraphicsPipeline* regionStencilCurveBlend_ = nullptr; // curve-boundary region see-through + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionSelectCurveMask_       = nullptr; // curve-boundary region gate (baked SDF mask), replace
    SDL_GPUGraphicsPipeline* regionSelectCurveMaskBlend_  = nullptr; // curve-mask region gate + premultiplied-over composite
    SDL_GPUGraphicsPipeline* regionStencilCurveMask_      = nullptr; // curve-boundary region see-through (baked SDF mask), replace
    SDL_GPUGraphicsPipeline* regionStencilCurveMaskBlend_ = nullptr; // curve-mask region see-through + premultiplied-over composite
    SDL_GPUGraphicsPipeline* blend_        = nullptr;  // programmable blend composite: applyBlendMode(dst, src, mode), replace
    SDL_GPUGraphicsPipeline* blit_         = nullptr;  // viewport → swapchain blit pipeline
    SDL_GPUSampler*          sampler_      = nullptr;  // nearest, clamped (tile atlas + faithful blit)
    SDL_GPUSampler*          bilinear_     = nullptr;  // linear, clamped (blit only; SamplingMode::Bilinear)
    SDL_GPUTexture*          paletteStore_ = nullptr;  // R16G16B16A16_UNORM store; PaletteId = flat colour offset
    SDL_GPUTexture*          atlasStore_   = nullptr;  // R32_UINT flat atlas store: all atlases
                                                       // stacked vertically; width = max atlas width,
                                                       // height = Σ heights; AtlasId → AtlasEntry::storeY
    int                      atlasStoreW_  = 0;         // store texture width (px); 0 = no atlas uploaded
    int                      atlasStoreH_  = 0;         // store texture height (px)
    SDL_GPUTexture*          atlasRegionStore_ = nullptr; // R32G32B32A32_UINT, one texel per AtlasId =
                                                          // (storeY, cols, transpMaskLo, transpMaskHi); the
                                                          // transparent-index set is a 64-bit bitmask split
                                                          // across .z (0–31) and .w (32–63). The global region
                                                          // table both frag stages index by a cell's / sprite's
                                                          // atlas handle
    std::vector<AtlasEntry>  atlases_;                 // indexed by AtlasId (region within atlasStore_)
    static const Renderer*   instance_;                // the one engine renderer (instance()), set at
                                                       // construction; the sprite shape query reads its atlases_
    std::vector<CurveMaskEntry> curveMasks_;           // indexed by CurveMaskId − 1 (1-based; 0 = none)
    std::vector<TilemapTex>  tilemaps_;                // indexed by frame.layers position
    std::vector<SpriteBuf>   spriteBufs_;              // indexed by frame.layers position
    // Per-run sprite-record buffers for MIXED-blend sprite layers (a layer whose sprites don't all share
    // BlendMode::Normal). An all-Normal layer keeps the single spriteBufs_ buffer + one instanced draw
    // (byte-identical); a mixed layer splits its draw order into contiguous same-blend runs (spriteBlendRuns)
    // and uploads each run's records to its own pool buffer, drawn with first_instance 0 — the region_batch
    // precedent: the sprite vertex stage carries no base-instance uniform, so uSprites[SV_InstanceID] is
    // 0-based and correct on every backend. A grow-on-demand pool assigned by a per-frame slot counter;
    // capacities in BYTES. Released with the sprite buffers.
    std::vector<SDL_GPUBuffer*> spriteRunBufs_;
    std::vector<int>            spriteRunCaps_;
    // One per-frame storage TEXTURE holding every sprite's flattened effect run (its effects chain + its
    // regions, packed as SpriteFxRecords). Each record is ten RGBA32F texels on one row (10 wide); a sprite's
    // GpuSprite.fxOffset/fxCount slice it by row, and the sprite fragment loops the slice inline. A storage
    // texture, not a storage buffer: a fragment storage buffer sits in Metal's [[buffer]] namespace after the
    // uniforms, so its index only matches the toolchain's when the texture and uniform counts coincide — a
    // storage texture (its own [[texture]] namespace) avoids that. Bound (t3 space2) on every sprite draw —
    // always at least one row (a dummy when the frame carries no sprite effects; never read, since those
    // sprites have fxCount 0), so the binding is always valid. Grow-on-demand by row count. Released with the
    // sprite buffers.
    SDL_GPUTexture*            spriteFxStore_ = nullptr;
    int                        spriteFxStoreRows_ = 0;
    std::vector<Rgba16>      paletteData_;             // CPU mirror of the store; flat, contiguous 16-bit palette colours (PaletteId = flat offset)
    SDL_GPUTexture*          rowDataStore_ = nullptr;  // per-frame RGBA32F data store: every effect's
                                                       // paramTable stacked vertically (width 1, one Vec4
                                                       // per row); a Custom effect Loads its rows from it.
                                                       // A 1×1 default always exists so the pipeline binds.
    std::vector<Vec4>        rowData_;                 // CPU mirror rebuilt each frame (the rows uploaded)
    int                      rowDataStoreH_ = 0;       // store texture height in rows; grows to fit
    // Registered custom shader stages, indexed by PostProcessStageId. Each stage builds
    // a pipeline PAIR from the game's fragment — replace (frame-level / Below scope) + premultiplied-over
    // blend (Layer scope) — mirroring displace_/displaceBlend_. A custom shader declares its OWN cbuffer;
    // customPackers_[id] (generated, custom_effect_packers.h) fills it from the effect's inline param
    // fields. Null packer = a parameterless shader (no uniform pushed). All three vectors stay parallel.
    std::vector<SDL_GPUGraphicsPipeline*> customReplace_;       // no-blend; one per registered stage
    std::vector<SDL_GPUGraphicsPipeline*> customBlend_;         // premultiplied-over; one per registered stage
    std::vector<EffectPacker>             customPackers_;       // cbuffer packer; one per registered stage
    // Instanced-additive region batching. customBatched_[id] is the stage's batched pipeline when its
    // shader carries `// @retropp:additive`, else nullptr (nullptr IS the "not additive" flag — the
    // renderer routes eligible same-shader regions through the batched pass only when it exists). Parallel
    // to the three vectors above. batchZeroSource_ is a 1×1 transparent-black texture bound as the batched
    // pass's SourceTexture — with a zero source, an additive shader returns exactly its source-independent
    // delta D, which the additive blend accumulates. batchInstanceBufs_ is a grow-on-demand pool of
    // per-run instance-record storage buffers (one bound per batched pass; first_instance 0, so
    // uRecords[SV_InstanceID] is correct on every backend — no vertex uniform, avoiding the Metal
    // storage+uniform [[buffer]] collision the sprite path documents).
    std::vector<SDL_GPUGraphicsPipeline*> customBatched_;       // instanced-additive; nullptr = not additive
    // Gathered-region rendering: customGather_[id] is the stage's union-shape REPLACE pipeline when its
    // shader has a gather variant (every custom shader EXCEPT additive- / no-gather-declared), else nullptr
    // (nullptr IS the "stage does not gather" flag). customGatherBlend_[id] is its premultiplied-over peer
    // (the Normal-layer last-step composite onto target_). Parallel to customReplace_/customBatched_. A
    // gather pass reads the previous image (real SourceTexture) + a fragment storage buffer of per-region
    // records (claimed from the SAME batchInstanceBufs_ pool as the additive runs) and writes the next image.
    std::vector<SDL_GPUGraphicsPipeline*> customGather_;        // replace; nullptr = stage does not gather
    std::vector<SDL_GPUGraphicsPipeline*> customGatherBlend_;   // premultiplied-over; parallel to customGather_
    // Sprite-inline rendering: customSprite_[id] is the stage's sprite-inline pipeline (the sprite fragment
    // with the custom body injected) when its shader has a sprite variant (every custom shader EXCEPT
    // no-sprite- / int-uint-param ones), else nullptr (the "stage can't run on a sprite" flag). Parallel to
    // customReplace_. A sprite carrying a Custom effect through this stage draws through this pipeline.
    std::vector<SDL_GPUGraphicsPipeline*> customSprite_;        // sprite-inline; nullptr = not on the sprite path
    // Below-custom rendering: customSpriteBelow_[id] is the stage's scene-facing sprite-inline pipeline (the
    // below sprite fragment with the custom body injected, sampleSource reading the scene) when its shader has
    // a sprite-below variant, else nullptr (the "no below-custom variant" flag). Parallel to customSprite_. A
    // sprite carrying a Below-scope Custom effect through this stage draws its lens through this pipeline.
    std::vector<SDL_GPUGraphicsPipeline*> customSpriteBelow_;    // below-custom; nullptr = no scene-read variant
    SDL_GPUTexture*                       batchZeroSource_ = nullptr;  // 1×1 transparent-black source
    std::vector<SDL_GPUBuffer*>           batchInstanceBufs_;    // per-run instance/gather records (pooled, grown)
    std::vector<int>                      batchInstanceCaps_;    // each pool buffer's capacity in BYTES (additive
                                                                 // records + gather blobs share the pool)
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
    SamplingMode             sampling_     = defaultSamplingMode;  // blit sampler; seeded by setActive()
    bool                     interpolation_ = defaultInterpolation;  // automatic interpolation; seeded by setActive()
    EvaluationGrid           evaluationGrid_ = defaultEvaluationGrid; // analytic-path evaluation grid; seeded by setActive()
    Interpolator             interp_;        // the per-id retained mirror (prev/cur tick state, by id)
};

}  // namespace retropp
