#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
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

// Stable, game-assigned layer identity. Lets a game reference the same logical layer
// across frames and makes a layer uniquely addressable within a frame. Opaque: the engine
// imposes no semantic roles; the game assigns whatever values it likes. Identity is THIS
// field — never the submission position.
enum class LayerId : std::uint32_t {};

// A handle to uploaded atlas pixel data the renderer owns. Identity is the typed handle;
// the renderer maps it to its GPU texture.
enum class AtlasId : std::uint32_t {};

// ── Tile content ────────────────────────────────────────────────────────────────────

// One cell of a tilemap: which atlas tile + which palette in the layer's set + flip. The
// opaque `attributes` byte (ENG-2.B.2.a) is replaced by named fields per the no-positional-
// opacity discipline — identity is a field, never a packed byte behind a comment. `palette`
// selects which palette WITHIN the layer's set (TileContent::palettes) this cell draws from —
// the mechanism that lets a 4-entry palette render a full-colour map. `priority` (BG-over-
// sprite) is deferred to ENG-2.B.2.c with sprites.
struct TileCell {
    std::uint16_t tile    = 0;      // index into the layer's INDEXED tile atlas
    std::uint8_t  palette = 0;      // which palette in the layer's set
    bool          flipX   = false;
    bool          flipY   = false;
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
//   [tile:16][palette:8][flipX:1][flipY:1][reserved:6]
// This constexpr pair is the unit-tested mirror of the GPU packing — the shader unpacks the
// identical layout, so packTileCell(unpackTileCell(w)) == w for every valid cell and the
// reserved bits leave room for `priority` in ENG-2.B.2.c without a re-upload-format change.
[[nodiscard]] constexpr std::uint32_t packTileCell(const TileCell& c) noexcept {
    return static_cast<std::uint32_t>(c.tile)
         | (static_cast<std::uint32_t>(c.palette) << 16)
         | (static_cast<std::uint32_t>(c.flipX ? 1u : 0u) << 24)
         | (static_cast<std::uint32_t>(c.flipY ? 1u : 0u) << 25);
}

[[nodiscard]] constexpr TileCell unpackTileCell(std::uint32_t packed) noexcept {
    TileCell c;
    c.tile    = static_cast<std::uint16_t>(packed & 0xFFFFu);
    c.palette = static_cast<std::uint8_t>((packed >> 16) & 0xFFu);
    c.flipX   = ((packed >> 24) & 1u) != 0u;
    c.flipY   = ((packed >> 25) & 1u) != 0u;
    return c;
}

// Sprite content — DECLARED here so the LayerContent variant is locked; placement +
// per-sprite attributes + rendering are realized in ENG-2.B.2.c.
struct SpriteContent {
    AtlasId atlas{};               // sprite pixel-data atlas (identity)
};

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

// ── Layer + frame ─────────────────────────────────────────────────────────────────────

struct LayerScroll { int x = 0; int y = 0; };

// One layer in the frame's arbitrary, Z-sorted stack. No semantic role is imposed by the
// engine. Runtime-dynamic: every field is fresh each frame.
struct DrawLayer {
    LayerId           id{};                 // stable identity — first member
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
//                    z → id → submission order anyway (a shipped game stays up).
enum class LayerKeyCollisionPolicy : std::uint8_t { Throw, WarnAndResolve };

// Default runtime policy, derived from build config: dev builds fail fast; release builds
// keep a shipped game running. Overridable at the call site / on the Renderer (the toggle).
inline constexpr LayerKeyCollisionPolicy kDefaultCollisionPolicy =
#ifdef NDEBUG
    LayerKeyCollisionPolicy::WarnAndResolve;
#else
    LayerKeyCollisionPolicy::Throw;
#endif

// Back-to-front draw order as indices into `layers`: ascending z, ties broken by ascending
// id, then submission order (stable). Returns indices so the caller composites without
// copying. Validates key uniqueness first and reacts per `policy` (see above); under
// WarnAndResolve the returned order is still fully deterministic. Throws std::invalid_argument
// on a collision under Throw.
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
