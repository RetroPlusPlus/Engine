#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "gbcpp/geometry.h"  // PixelSize
#include "gbcpp/palette.h"   // PaletteId

namespace gbcpp {

// The draw-state submission envelope: the C++ shape a game hands the renderer each frame.
//
// The frame's draw state is computed WHOLE every frame from the game's logical inputs —
// there is no mid-frame state-change API, no reconstructed scanline ISR, no register poke.
// A game recomputes and resubmits a fresh FrameDrawState per frame; layer existence, z,
// scroll, size, alpha, and effect/modifier parameters are all fresh each frame (the one
// amortized exception is the atlas texels, uploaded when they change). Effects that a GB
// expressed through hardware tricks are expressed here as layers, per-tile/per-region
// colour attributes, frame-level modifiers, and screen-space-effect declarations — never
// as a hardware-register idiom.
//
// Identity is a typed, first-class field throughout (LayerId, AtlasId, the *Kind enums) —
// never an array position, never demoted to a comment.

// ── Identity handles ────────────────────────────────────────────────────────────────

// Stable, game-assigned layer identity — a human-readable LABEL, not a number. A game names its
// layers ("BottomSpriteLayer", "ScrollingParallaxClouds", …) and references the same logical layer
// across frames by that name. The engine imposes no roles and never derives meaning from the name;
// it only uses it to tell layers apart and to name them in diagnostics. Identity is THIS field —
// never the submission position — and it is fully INDEPENDENT of z: z alone controls depth, the id
// plays no part in ordering. Compared by name. Construct implicitly from a string literal:
//   layer.id = "BottomSpriteLayer";
// The name must outlive the renderFrame() call that consumes it — string literals always do (static
// storage); a dynamically-built name follows the same game-owned-data lifetime as the content spans.
struct LayerId {
    std::string_view name;
    constexpr LayerId() noexcept = default;
    constexpr LayerId(const char* n) noexcept : name(n) {}        // implicit: id = "Foo"
    constexpr LayerId(std::string_view n) noexcept : name(n) {}
    [[nodiscard]] constexpr bool operator==(const LayerId&) const noexcept = default;
};

// A handle to uploaded atlas pixel data the renderer owns. Identity is the typed handle;
// the renderer maps it to its GPU texture.
enum class AtlasId : std::uint32_t {};

// ── Tile content ────────────────────────────────────────────────────────────────────

// One cell of a tilemap: which atlas tile + which palette in the layer's set + flip. The
// opaque `attributes` byte (ENG-2.B.2.a) is replaced by named fields per the no-positional-
// opacity discipline — identity is a field, never a packed byte behind a comment. `palette`
// selects which palette WITHIN the layer's set (TileContent::palettes) this cell draws from —
// the mechanism that lets a 4-entry palette render a full-colour map. `priority` (BG-over-OBJ)
// is carried here so the cell layout is final; its cross-layer interaction with sprite priority
// is realized in ENG-2.B.2.c.2 — the tile shader reads bits 0..25 only and ignores it.
struct TileCell {
    std::uint16_t tile     = 0;     // index into the layer's INDEXED tile atlas
    std::uint8_t  palette  = 0;     // which palette in the layer's set
    bool          flipX    = false;
    bool          flipY    = false;
    bool          priority = false; // BG-over-OBJ; carried — interaction realized in B.2.c.2
};

// A tile layer's content: an INDEXED tile atlas (one palette index per pixel), the layer's
// palette set (the bank a cell's `palette` selects within), and a row-major tilemap
// (widthInTiles × heightInTiles). The map is sampled per-pixel in the tile shader against the
// layer's scroll, so arbitrary layer sizes and wrapping are handled on the GPU. `atlas`,
// `palettes`, and `cells` are game-owned; valid for the duration of the renderFrame() call
// that consumes them. A palette set of one is the single-palette case.
struct TileContent {
    AtlasId                    atlas{};         // indexed tile atlas (palette indices, not colour)
    std::span<const PaletteId> palettes;        // the layer's palette set; TileCell::palette selects within
    int                        widthInTiles  = 0;
    int                        heightInTiles = 0;
    std::span<const TileCell>  cells;           // row-major, widthInTiles * heightInTiles
};

// The R32_UINT tilemap cell layout the tile fragment shader unpacks:
//   [tile:16][palette:8][flipX:1][flipY:1][priority:1][reserved:5]
// This constexpr pair is the unit-tested mirror of the GPU packing — the shader unpacks the
// identical layout, so packTileCell(unpackTileCell(w)) == w for every valid cell. `priority`
// claims bit 26 (the first prior-reserved bit); the tile fragment shader still reads only bits
// 0..25, so adding the field is byte-transparent to the tile path (a priority=false cell packs
// identically to the pre-ENG-2.B.2.c layout).
[[nodiscard]] constexpr std::uint32_t packTileCell(const TileCell& c) noexcept {
    return static_cast<std::uint32_t>(c.tile)
         | (static_cast<std::uint32_t>(c.palette) << 16)
         | (static_cast<std::uint32_t>(c.flipX    ? 1u : 0u) << 24)
         | (static_cast<std::uint32_t>(c.flipY    ? 1u : 0u) << 25)
         | (static_cast<std::uint32_t>(c.priority ? 1u : 0u) << 26);
}

[[nodiscard]] constexpr TileCell unpackTileCell(std::uint32_t packed) noexcept {
    TileCell c;
    c.tile     = static_cast<std::uint16_t>(packed & 0xFFFFu);
    c.palette  = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
    c.flipX    = ((packed >> 24) & 1u) != 0u;
    c.flipY    = ((packed >> 25) & 1u) != 0u;
    c.priority = ((packed >> 26) & 1u) != 0u;
    return c;
}

// ── Sprite content ────────────────────────────────────────────────────────────────────

// A sprite's pixel dimensions. Identity is the named fields. Console sprite sizes are named
// presets — static members of the type (SpriteSize::GameBoy8x8, …), the self-type-constant
// idiom (declared in-class, defined inline constexpr just below), byte-for-byte the
// ViewportResolution / TimingProfile pattern. A size IS a {width, height} tuple, so a preset and
// a raw value are interchangeable. The preset names carry their dimensions (GameBoy8x16, not
// "GameBoyTall") so the value is legible at the call site. Not an exhaustive registry; the engine
// generalizes beyond the Game Boy, so an arbitrary SpriteSize{w,h} covers anything not named.
struct SpriteSize {
    int width  = 8;
    int height = 8;
    [[nodiscard]] constexpr bool operator==(const SpriteSize&) const noexcept = default;

    static const SpriteSize GameBoy8x8;        // default when nothing is specified
    static const SpriteSize GameBoy8x16;
    static const SpriteSize GameBoyColor8x8;
    static const SpriteSize GameBoyColor8x16;
    static const SpriteSize GameBoyAdvance8x8; // GBA base; OBJ range 8×8…64×64
    static const SpriteSize Nes8x8;
    static const SpriteSize Nes8x16;
    static const SpriteSize MasterSystem8x8;
    static const SpriteSize MasterSystem8x16;
    static const SpriteSize Snes8x8;
    static const SpriteSize Snes16x16;
    static const SpriteSize Snes32x32;
    static const SpriteSize Snes64x64;
    static const SpriteSize Genesis32x32;      // max single sprite; MD composes 8px cells
};

inline constexpr SpriteSize SpriteSize::GameBoy8x8{8, 8};
inline constexpr SpriteSize SpriteSize::GameBoy8x16{8, 16};
inline constexpr SpriteSize SpriteSize::GameBoyColor8x8{8, 8};
inline constexpr SpriteSize SpriteSize::GameBoyColor8x16{8, 16};
inline constexpr SpriteSize SpriteSize::GameBoyAdvance8x8{8, 8};
inline constexpr SpriteSize SpriteSize::Nes8x8{8, 8};
inline constexpr SpriteSize SpriteSize::Nes8x16{8, 16};
inline constexpr SpriteSize SpriteSize::MasterSystem8x8{8, 8};
inline constexpr SpriteSize SpriteSize::MasterSystem8x16{8, 16};
inline constexpr SpriteSize SpriteSize::Snes8x8{8, 8};
inline constexpr SpriteSize SpriteSize::Snes16x16{16, 16};
inline constexpr SpriteSize SpriteSize::Snes32x32{32, 32};
inline constexpr SpriteSize SpriteSize::Snes64x64{64, 64};
inline constexpr SpriteSize SpriteSize::Genesis32x32{32, 32};

// One placed sprite. `x`/`y` are the top-left in the LAYER's coordinate space (before scroll —
// the vertex shader subtracts the layer scroll, so a sprite on a world-scroll layer tracks the
// background, and a HUD layer at scroll {0,0} stays fixed). `tile` is the top-left atlas cell
// (8px grid); the sprite reads a size.width × size.height pixel rectangle from the atlas at that
// cell's pixel origin (so a 16×16 sprite spans a 2×2 cell block laid out contiguously). `palette`
// selects which palette WITHIN the layer's set this sprite colours through. `priority` (behind-BG)
// is carried here; its cross-layer interaction is realized in ENG-2.B.2.c.2 (B.2.c.1 front-
// composites all sprites by layer z). Identity is the named fields — no packed attribute byte.
struct Sprite {
    int           x        = 0;
    int           y        = 0;
    SpriteSize    size     = SpriteSize::GameBoy8x8;
    std::uint16_t tile     = 0;       // top-left atlas cell
    std::uint8_t  palette  = 0;       // palette-select within the layer's set
    bool          flipX    = false;
    bool          flipY    = false;
    bool          priority = false;   // behind-BG; carried — interaction realized in B.2.c.2
};

// A sprite layer's content: an INDEXED atlas (shared with the tile path's atlas registry), the
// layer's palette set (the bank a sprite's `palette` selects within), and the layer's sprites.
// `atlas`, `palettes`, and `sprites` are game-owned; valid for the duration of the renderFrame()
// call. Mirrors TileContent. An empty `sprites` span is a valid (degenerate) submission.
struct SpriteContent {
    AtlasId                    atlas{};         // indexed sprite atlas (palette indices, not colour)
    std::span<const PaletteId> palettes;        // the layer's palette set; Sprite::palette selects within
    std::span<const Sprite>    sprites;         // the layer's placed sprites
};

// The sprite storage-buffer record the sprite VERTEX shader reads (one per sprite). std430-style
// 16-byte alignment → 32 bytes, laid out as the shader's { float4 clip; uint4 attr; }:
//   clip = (clipX, clipY, clipW, clipH) — the sprite quad's top-left in CLIP space plus its clip-
//          space span; corner c maps to clip (clipX + c.x*clipW, clipY + c.y*clipH). clipH is
//          negative (the top-left-origin V-flip), so the vertex shader needs no viewport/scroll
//          uniform — the whole screen→clip transform (scroll subtraction + viewport scale + V-flip)
//          is baked CPU-side in makeGpuSprite. This is deliberate: a vertex stage carrying both a
//          storage buffer AND a uniform buffer collides in Metal's [[buffer]] namespace (SDL_GPU
//          offsets storage buffers past the uniform buffers, but the single-pass HLSL→SPIR-V→MSL
//          toolchain can't express that and Vulkan's descriptor layout simultaneously). Baking the
//          transform leaves the vertex stage with ONE buffer. See PLAN Amendment A2.
//   attr = (tile, paletteRow, flags, size): `paletteRow` is the RESOLVED palette-store row
//          (resolved CPU-side from the layer's set + the sprite's select); `flags` is packSpriteFlags;
//          `size` is the pixel size packed (width<<16)|height for the fragment's within-sprite
//          addressing. This is the unit-tested CPU↔GPU mirror, same discipline as packTileCell.
struct GpuSprite {
    float         clipX, clipY;  // sprite quad top-left, clip space (scroll applied, V-flipped)
    float         clipW, clipH;  // clip-space span; clipH is negative (top-left-origin V-flip)
    std::uint32_t tile;          // top-left atlas cell
    std::uint32_t paletteRow;    // resolved palette-store row
    std::uint32_t flags;         // bit0 flipX | bit1 flipY | bit2 priority
    std::uint32_t size;          // pixel size packed (width<<16)|height
};
static_assert(sizeof(GpuSprite) == 32);

[[nodiscard]] constexpr std::uint32_t packSpriteFlags(bool flipX, bool flipY, bool priority) noexcept {
    return (flipX ? 1u : 0u) | (flipY ? 2u : 0u) | (priority ? 4u : 0u);
}

// Pack a sprite's pixel dimensions into one uint (width in the high 16 bits). The fragment shader
// unpacks this to map the interpolated within-sprite UV back to an atlas pixel.
[[nodiscard]] constexpr std::uint32_t packSpriteSize(const SpriteSize& sz) noexcept {
    return (static_cast<std::uint32_t>(sz.width) << 16) | static_cast<std::uint32_t>(sz.height & 0xFFFF);
}

// Resolve a sprite's palette-select to a palette-store row via the layer's set (mirrors the tile
// path's paletteSetRows mapping; a PaletteId's underlying value == its store row). An out-of-range
// select or an empty set resolves to row 0 (degenerate but valid).
[[nodiscard]] constexpr std::uint32_t spritePaletteRow(std::span<const PaletteId> set,
                                                       std::uint8_t select) noexcept {
    return select < set.size() ? static_cast<std::uint32_t>(set[select]) : 0u;
}

// Build the GPU record for one sprite. `viewportW`/`viewportH` are the offscreen viewport pixel
// size; `scrollX`/`scrollY` the layer scroll. The sprite's top-left is (x − scroll); the screen→clip
// transform (viewport scale + top-left-origin V-flip) is baked here so the vertex shader is a pure
// storage-buffer read (no uniform). Pure + constexpr — the unit-tested CPU↔GPU mirror.
[[nodiscard]] constexpr GpuSprite makeGpuSprite(const Sprite& s, std::uint32_t paletteRow,
                                                int viewportW, int viewportH,
                                                int scrollX, int scrollY) noexcept {
    const float vw  = static_cast<float>(viewportW);
    const float vh  = static_cast<float>(viewportH);
    const float sox = static_cast<float>(s.x - scrollX);  // screen-space top-left
    const float soy = static_cast<float>(s.y - scrollY);
    GpuSprite g{};
    g.clipX      = sox / vw * 2.0f - 1.0f;
    g.clipW      = static_cast<float>(s.size.width)  / vw * 2.0f;
    g.clipY      = 1.0f - soy / vh * 2.0f;                       // top-left origin
    g.clipH      = -(static_cast<float>(s.size.height) / vh * 2.0f);
    g.tile       = s.tile;
    g.paletteRow = paletteRow;
    g.flags      = packSpriteFlags(s.flipX, s.flipY, s.priority);
    g.size       = packSpriteSize(s.size);
    return g;
}

// A layer carries exactly one content alternative. The active alternative is the variant's
// identity; LayerContentKind mirrors it for explicit, switch-friendly dispatch.
enum class LayerContentKind : std::uint8_t { Tiles, Sprites };
using LayerContent = std::variant<TileContent, SpriteContent>;
[[nodiscard]] constexpr LayerContentKind contentKind(const LayerContent& c) noexcept {
    return c.index() == 0 ? LayerContentKind::Tiles : LayerContentKind::Sprites;
}

// ── Screen-space effects (type seam locked here; shader realization is ENG-2.C) ───────

enum class Axis : std::uint8_t { Horizontal, Vertical };

enum class ScreenSpaceEffectKind : std::uint8_t {
    None,            // pass-through — the ENG-2.B.2.a default; realization is ENG-2.C
    RowDisplacement, // wavy water / heat haze / per-line SCX = f(row, time) in a shader
};

// What a displacement does at the frame edge, where a row/column pulled inward exposes a strip with
// no source pixel behind it. Developer-selectable per effect (ENG-2.C.2.a Amendment A3):
//   Blank   — the exposed strip is the backdrop colour (nothing there). The faithful default: a
//             whole-frame displacement has no off-screen content to reveal, so it shows blank.
//   Stretch — the edge pixel is duplicated outward (CLAMP_TO_EDGE), smearing the border colour.
enum class DisplacementEdge : std::uint8_t { Blank, Stretch };

// Which pixels a per-layer effect transforms — the composable Photoshop-layer model (ENG-2.C.2.b).
// (Meaningful for DrawLayer::effect; FrameDrawState::postEffects is inherently whole-frame and
// ignores it.)
//   Layer — ISOLATED: displace ONLY this layer's own content, before it composites. A wavy water
//           layer distorts while the layers/sprites composited above it stay still. The default.
//   Below — ADJUSTMENT LAYER: displace the WHOLE accumulated image at this layer's z — this layer's
//           own content AND everything beneath it, coherently — then layers above this z composite
//           on top, undisplaced. A content-less Below layer just under a HUD wobbles the world while
//           the HUD rides steady; a content-bearing Below layer wobbles itself together with the
//           scene beneath. Multiple Below effects compose by z.
enum class ScreenSpaceEffectScope : std::uint8_t { Layer, Below };

// A screen-space effect declaration. ENG-2.B.2.a locks the type + carries the parameters
// as data; the shader stage that interprets them is ENG-2.C / Issue 5. In screen space the
// fragment's row coordinate IS the scanline, so a continuous effect is a function
// f(row, time, frame-state) the GPU evaluates per-pixel — no reconstructed LY counter, no
// HBlank ISR. The game advances `phase` per frame to animate (runtime-dynamic).
struct ScreenSpaceEffect {
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // identity, first member
    float amplitude = 0.0f;   // displacement magnitude, viewport pixels
    float frequency = 0.0f;   // cycles across the displaced axis
    float phase     = 0.0f;   // animation phase (game advances off frame time)
    Axis  axis      = Axis::Horizontal;
    DisplacementEdge edge = DisplacementEdge::Blank;  // frame-edge behaviour; Blank is the default
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach; Layer (isolated) default
};

// ── Frame-level modifiers (types locked here; output realization is ENG-2.B.2.c) ──────

enum class ColorModifierKind : std::uint8_t { None, MultiplyAdd };

// Day/night-style whole-frame colour transform: out = clamp(in * mul + add). Default is the
// identity transform (None / mul=1 / add=0).
struct ColorModifier {
    ColorModifierKind kind = ColorModifierKind::None;  // identity, first member
    float mulR = 1.0f, mulG = 1.0f, mulB = 1.0f;
    float addR = 0.0f, addG = 0.0f, addB = 0.0f;
};

enum class BlendKind : std::uint8_t { None, Flash };

// Frame-level blend (cutscene flash): mix the composed frame toward (r,g,b) by `strength`.
struct Blend {
    BlendKind kind = BlendKind::None;  // identity, first member
    float r = 0.0f, g = 0.0f, b = 0.0f, strength = 0.0f;
};

// The blit-stage transform a frame's globalModifier + blend resolve to: a whole-frame post-
// composite transform on already-composited pixels — out = clamp(in*mul + add), then mix toward
// (flashR,flashG,flashB) by flashStrength. Default is the IDENTITY (mul=1, add=0, strength=0) →
// the blit is byte-identical to the ENG-2.B.2.c.1 pass-through. The unit-tested CPU mirror of the
// blit fragment shader's math (discipline of packTileCell / makeGpuSprite). NOT the colouring
// mechanism (that is index + palette) — this is the modern post-composite effect transform for
// whole-frame fades / day-night / cutscene flash (GB_PORT_ENGINE_DECISIONS.md § "Colour model").
struct FrameColorTransform {
    float mulR = 1.0f, mulG = 1.0f, mulB = 1.0f;
    float addR = 0.0f, addG = 0.0f, addB = 0.0f;
    float flashR = 0.0f, flashG = 0.0f, flashB = 0.0f;
    float flashStrength = 0.0f;
    [[nodiscard]] constexpr bool operator==(const FrameColorTransform&) const noexcept = default;
};

// Resolve a frame's ColorModifier + Blend to the blit-stage transform. None on either input
// leaves that part identity, so a default frame yields FrameColorTransform{} (the identity) and
// the blit renders the faithful baseline byte-for-byte. flashStrength is clamped to [0,1] here so
// the GPU receives a valid value and this helper stays the authoritative mirror.
[[nodiscard]] constexpr FrameColorTransform
frameColorTransform(const ColorModifier& m, const Blend& b) noexcept {
    FrameColorTransform t;  // identity
    if (m.kind == ColorModifierKind::MultiplyAdd) {
        t.mulR = m.mulR; t.mulG = m.mulG; t.mulB = m.mulB;
        t.addR = m.addR; t.addG = m.addG; t.addB = m.addB;
    }
    if (b.kind == BlendKind::Flash) {
        t.flashR = b.r; t.flashG = b.g; t.flashB = b.b;
        t.flashStrength = b.strength < 0.0f ? 0.0f : (b.strength > 1.0f ? 1.0f : b.strength);
    }
    return t;
}

// ── Layer + frame ─────────────────────────────────────────────────────────────────────

struct LayerScroll { int x = 0; int y = 0; };

// One layer in the frame's arbitrary, Z-sorted stack. No semantic role is imposed by the
// engine. Runtime-dynamic: every field is fresh each frame.
struct DrawLayer {
    LayerId           id{};                 // stable identity label — first member; no role in depth
    std::int32_t      z = 0;                // back-to-front sort key; unique within a frame
    PixelSize         size{};               // independent per-layer dimensions
    LayerScroll       scroll{};             // independent scroll offset
    float             alpha = 1.0f;         // [0,1], default opaque
    LayerContent      content{ TileContent{} };
    ScreenSpaceEffect effect{};             // per-layer; None by default
};

// The whole frame's draw state. The game clears() + refills `layers` each frame (clear()
// preserves capacity → arbitrary N with no steady-state heap churn). This is RUNTIME engine
// state, not ROM data — the BoundedVec fixed-cap idiom does not apply.
struct FrameDrawState {
    std::vector<DrawLayer>         layers;           // arbitrary N; compositor stable-sorts by z
    ColorModifier                  globalModifier{}; // day/night; None by default (B.2.c)
    Blend                          blend{};          // cutscene flash; None by default (B.2.c)
    std::vector<ScreenSpaceEffect> postEffects;      // frame-level; carried, realized in 2.C
};

// ── Pure helpers (headlessly unit-tested) ─────────────────────────────────────────────

[[nodiscard]] constexpr float clampAlpha(float a) noexcept {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// The tile shader's per-layer palette-set → store-row uniform has a fixed slot count; a
// TileCell::palette selects slot 0..K-1. K=16 covers GB's 8 BG palettes with headroom. A
// palette set larger than K (resolved via a storage buffer) is a forward option, not built
// now — cells can only select 0..K-1.
inline constexpr std::size_t kPaletteSetSlots = 16;

// Resolve a layer's palette set to the per-layer uSetRows uniform: slot i holds the palette-
// store row of palettes[i] (a PaletteId's underlying value IS its store row), 0 for slots
// beyond the set. Pure mirror of the compositor's per-layer uniform fill — unit-tested. An
// empty set yields all-zero rows (a degenerate but valid submission); a set longer than K is
// truncated to the first K.
[[nodiscard]] constexpr std::array<std::uint32_t, kPaletteSetSlots>
paletteSetRows(std::span<const PaletteId> set) noexcept {
    std::array<std::uint32_t, kPaletteSetSlots> rows{};
    const std::size_t n = std::min(set.size(), kPaletteSetSlots);
    for (std::size_t i = 0; i < n; ++i) {
        rows[i] = static_cast<std::uint32_t>(set[i]);
    }
    return rows;
}

// --- Layer-key collision detection (compile-time-capable) ---

// A detected violation of layer-key uniqueness within one frame. Two distinct layers MUST
// NOT share a z (their front-to-back order would be undefined) nor a LayerId (identity must
// be unambiguous). `kind` is the identity — first member.
struct LayerKeyCollision {
    enum class Kind : std::uint8_t { DuplicateZ, DuplicateId };
    Kind         kind;    // which invariant was violated — identity, first member
    LayerId      first;   // DuplicateZ: the earlier layer's id; DuplicateId: the shared id
    LayerId      second;  // DuplicateZ: the later layer's id;   DuplicateId: == first
    std::int32_t z;       // DuplicateZ: the shared z;           DuplicateId: first layer's z
};

// Scan a layer set for the first key collision (duplicate z OR duplicate id). constexpr and
// pure, so a static_assert over a compile-time-known layer set turns a fixed layer stack's
// collision into a BUILD error — caught before the game ever runs. Returns nullopt when the
// keys are unique. O(n²); the layer population is small (compositing planes, not per-sprite
// primitives).
[[nodiscard]] constexpr std::optional<LayerKeyCollision>
findLayerKeyCollision(std::span<const DrawLayer> layers) noexcept {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        for (std::size_t j = i + 1; j < layers.size(); ++j) {
            if (layers[i].id == layers[j].id) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateId,
                                         layers[i].id, layers[j].id, layers[i].z};
            }
            if (layers[i].z == layers[j].z) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateZ,
                                         layers[i].id, layers[j].id, layers[i].z};
            }
        }
    }
    return std::nullopt;
}

// True when no two layers share a z or a LayerId. constexpr — the static_assert seam:
//   static_assert(layerKeysAreUnique(kMyFixedLayers), "z/id collision in layer stack");
// gives compile-time detection for any layer set known at compile time.
[[nodiscard]] constexpr bool layerKeysAreUnique(std::span<const DrawLayer> layers) noexcept {
    return !findLayerKeyCollision(layers).has_value();
}

// --- Runtime draw order + collision policy ---

// How layerDrawOrder() reacts to a key collision detected at RUNTIME. Orthogonal to the
// compile-time static_assert path (layerKeysAreUnique), which is always available.
//   Throw          — throw std::invalid_argument naming the colliding keys (fail fast; the
//                    development default, so a mistake surfaces the instant its frame runs).
//   WarnAndResolve — log a warning naming the colliding keys, then return the deterministic
//                    z → submission order anyway (a shipped game stays up).
enum class LayerKeyCollisionPolicy : std::uint8_t { Throw, WarnAndResolve };

// Default runtime policy, derived from build config: dev builds fail fast; release builds
// keep a shipped game running. Overridable at the call site / on the Renderer (the toggle).
inline constexpr LayerKeyCollisionPolicy kDefaultCollisionPolicy =
#ifdef NDEBUG
    LayerKeyCollisionPolicy::WarnAndResolve;
#else
    LayerKeyCollisionPolicy::Throw;
#endif

// Back-to-front draw order as indices into `layers`: ascending z (the sole depth key — the id is
// a pure label with no ordering role), equal z falling back to submission order (stable). Returns
// indices so the caller composites without copying. Validates key uniqueness first and reacts per
// `policy` (see above); under WarnAndResolve the returned order is still fully deterministic.
// Throws std::invalid_argument on a collision under Throw.
[[nodiscard]] std::vector<std::size_t>
layerDrawOrder(std::span<const DrawLayer> layers,
               LayerKeyCollisionPolicy policy = kDefaultCollisionPolicy);

// --- Tilemap sampling math (mirrors the tile fragment shader) ---

struct TileSample { int tileX; int tileY; int pixelX; int pixelY; };

namespace detail {
// Floor-division / floor-modulo so negative scroll wraps correctly (C++ `/` and `%`
// truncate toward zero, which would mis-wrap negative world coordinates).
[[nodiscard]] constexpr int floorDiv(int a, int b) noexcept {
    const int q = a / b;
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
[[nodiscard]] constexpr int floorMod(int a, int n) noexcept {
    const int r = a % n;
    return (r < 0) ? r + n : r;
}
}  // namespace detail

// Given an output pixel within the layer (origin top-left, before scroll), the layer's
// scroll, and the tilemap dimensions, return the wrapped tile coordinate and the within-tile
// pixel offset. Pure + constexpr so the (scroll, wrap, negative-scroll) mapping is unit-
// testable independent of the GPU; the tile fragment shader runs the identical math.
// Precondition: tilePx > 0. Degenerate (≤0) tilemap dimensions yield tile coord 0 on that
// axis rather than dividing by zero.
[[nodiscard]] constexpr TileSample sampleTilemap(int px, int py, LayerScroll scroll,
                                                 int widthInTiles, int heightInTiles,
                                                 int tilePx = 8) noexcept {
    const int worldX = px + scroll.x;
    const int worldY = py + scroll.y;
    const int tileX  = widthInTiles  > 0 ? detail::floorMod(detail::floorDiv(worldX, tilePx), widthInTiles)  : 0;
    const int tileY  = heightInTiles > 0 ? detail::floorMod(detail::floorDiv(worldY, tilePx), heightInTiles) : 0;
    const int pixelX = detail::floorMod(worldX, tilePx);
    const int pixelY = detail::floorMod(worldY, tilePx);
    return TileSample{tileX, tileY, pixelX, pixelY};
}

}  // namespace gbcpp
