#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/animation.h"    // AnimationFrame (the AtlasManifest::frame shorthand returns one)
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

// The result of loading + slicing an atlas image: the uploaded atlas handle plus the
// carved sub-asset slots in read order. `manifest[i]` is the i-th carved asset's slot (its top-left
// atlas cell + dimensions) — feed slot.tile to a TileCell::tile / Sprite::tile and slot.dimensions
// to a Sprite::size. It carries the AtlasId (so it lives here, where AtlasId + the GPU do); the slots
// are pure geometry from sliceLayout. The atlas uploads ONCE — re-slicing for a different order/kind
// is a pure sliceLayout call against the same AtlasId, no re-upload.
struct AtlasManifest {
    AtlasId                atlas{};
    std::vector<AssetSlot> slots;
    // >0 only for an AnimationSeries load (the grid holds MULTIPLE animations, this many frames each);
    // 0 = ungrouped (Single / Tileset / SpriteSeries / SingleAnimation). The flat carve is unchanged;
    // this just records how the contiguous slots divide into per-animation runs (groupCount/group).
    int framesPerAnimation = 0;

    [[nodiscard]] std::size_t      count() const noexcept { return slots.size(); }
    [[nodiscard]] const AssetSlot& operator[](std::size_t i) const { return slots[i]; }

    // The manifest stands in for its atlas where an AtlasId is wanted (e.g. TileCatalogEntry::sheet,
    // TileContent::atlas), so you write `layer.atlas = sheet` instead of `sheet.atlas`. Implicit by
    // design — a manifest IS one uploaded atlas (plus its slots). Slots stay explicit via operator[].
    [[nodiscard]] constexpr operator AtlasId() const noexcept { return atlas; }

    // AnimationSeries navigation. groupCount() = how many whole per-animation runs the slots hold
    // (slots / framesPerAnimation; 0 when ungrouped or fewer slots than one run). group(g) = the g-th
    // animation's contiguous run of framesPerAnimation slots, in read order — feed it (with the atlas
    // + a palette + a duration) straight into an Animation. Throws std::out_of_range if g >= groupCount().
    [[nodiscard]] std::size_t groupCount() const noexcept {
        return framesPerAnimation > 0
                   ? slots.size() / static_cast<std::size_t>(framesPerAnimation)
                   : 0;
    }
    [[nodiscard]] std::span<const AssetSlot> group(std::size_t g) const {
        if (g >= groupCount()) {
            throw std::out_of_range("AtlasManifest::group: group index out of range");
        }
        const std::size_t per = static_cast<std::size_t>(framesPerAnimation);
        return std::span<const AssetSlot>(slots.data() + g * per, per);
    }

    // Build an AnimationFrame for this sheet's `cell`-th slot — the shorthand for the common case where
    // an animation's frames all come from ONE loaded sheet: it fills in the frame's `.atlas` (this
    // manifest's) and `.slot` (slots[cell]) so the call site supplies just the cell index, a palette,
    // a duration, and an optional label. A frame can always be written as a full AnimationFrame literal
    // instead, pointing `.atlas`/`.slot` at a DIFFERENT sheet for multi-sheet animations — this is the
    // shortcut, not the only way. `label` is optional (empty = unnamed).
    [[nodiscard]] AnimationFrame frame(std::size_t cell, PaletteId palette,
                                       std::chrono::nanoseconds duration,
                                       std::string_view label = {}) const {
        return AnimationFrame{label, atlas, (*this)[cell], palette, duration};
    }
};

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
    // setSamplingMode() remains the per-renderer runtime override.
    static inline SamplingMode defaultSamplingMode = SamplingMode::Nearest;

    // The settable default for automatic interpolation — seeded by EngineConfig::setActive() from
    // EngineConfig::interpolation so a bare `Renderer{device, window}` inherits the host's choice.
    // Initializes to true (the smooth baseline). setInterpolation() is the per-renderer runtime override.
    static inline bool defaultInterpolation = true;

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
    // manifest.group(g) yields each animation's run. It is consulted ONLY for ContentKind::AnimationSeries
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
    // There is no interpolation argument: when interpolation is on (the default), the renderer reads the
    // run loop's sub-tick factor + tick signal from the frame-timing channel (frame_timing.h), reconciles
    // this submission into its per-id retained mirror once per tick, and composites each layer/sprite eased
    // between its previous and current tick state — so the game submits its latest state and the engine
    // blends. With interpolation off (setInterpolation(false)) the submission composites verbatim.
    void renderFrame(const FrameDrawState& frame);

    // Automatic interpolation, runtime-dynamic. A renderer starts at defaultInterpolation (seeded from
    // EngineConfig::interpolation by setActive()); this toggles it on the fly. On: ease each object between
    // its previous and current sim-tick state by the loop's sub-tick factor (the per-id mirror tracks the
    // last two ticks). Off: composite each submission exactly as given (no mirror, no blend).
    void               setInterpolation(bool enabled) noexcept { interpolation_ = enabled; }
    [[nodiscard]] bool interpolationEnabled() const noexcept { return interpolation_; }

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
    void setSamplingMode(SamplingMode mode) noexcept { sampling_ = mode; }
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
    struct SpriteBuf { SDL_GPUBuffer* buffer = nullptr; int capacity = 0; };

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
    SDL_GPUTexture* composeViewport(SDL_GPUCommandBuffer* cmd, const FrameDrawState& frame,
                                    std::vector<SDL_GPUTransferBuffer*>& scratch);

    void releaseAtlases();
    void releaseTilemaps();
    void releaseSpriteBuffers();
    void releaseCustomStages();

    SDL_GPUDevice*           device_;
    SDL_Window*              window_;
    ViewportResolution       viewport_;
    // The compose grid content is rasterized onto: viewport × composeScale_. Held distinct from the
    // viewport's normalization role — effects and regions measure against the viewport (invViewportW/H,
    // region radii), while offscreen-target sizing and content placement use the compose grid. At
    // composeScale_ == 1 the two coincide. composeW_/composeH_ are resolved from these in the ctor.
    int                      composeScale_ = 1;
    int                      composeW_     = 0;   // viewport_.width  * composeScale_
    int                      composeH_     = 0;   // viewport_.height * composeScale_
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
    SDL_GPUGraphicsPipeline* displace_     = nullptr;  // row-displacement post-process stage
    SDL_GPUGraphicsPipeline* displaceBlend_ = nullptr; // displace + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* ripple_       = nullptr;  // built-in radial ripple post-process stage
    SDL_GPUGraphicsPipeline* rippleBlend_  = nullptr;  // ripple + premultiplied-over composite (Layer scope)
    SDL_GPUGraphicsPipeline* colorFill_      = nullptr; // built-in colour fill / tint post-process stage
    SDL_GPUGraphicsPipeline* colorFillBlend_ = nullptr; // colorFill + premultiplied-over composite (Layer scope)
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
    std::vector<CurveMaskEntry> curveMasks_;           // indexed by CurveMaskId − 1 (1-based; 0 = none)
    std::vector<TilemapTex>  tilemaps_;                // indexed by frame.layers position
    std::vector<SpriteBuf>   spriteBufs_;              // indexed by frame.layers position
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
    LayerKeyCollisionPolicy  collisionPolicy_ = kDefaultCollisionPolicy;
    SamplingMode             sampling_     = defaultSamplingMode;  // blit sampler; seeded by setActive()
    bool                     interpolation_ = defaultInterpolation;  // automatic interpolation; seeded by setActive()
    Interpolator             interp_;        // the per-id retained mirror (prev/cur tick state, by id)
};

}  // namespace retropp
