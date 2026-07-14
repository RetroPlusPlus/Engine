// Shape-query demo — a sprite's own silhouette as a shape, in all three forms, driven by the mouse.
//
// The sprite IS the shape. A lumpy blob (with a concave notch AND an interior hole) follows the cursor,
// auto-cycling its flips / rotation and swaying under a geometric transform. Nothing re-draws the
// silhouette — the blob's own art is the exact silhouette. The three forms show up like this:
//
//   • asShape() — the LIVE borrow. The scattered coins turn GREEN where a VISIBLE pixel of the blob covers
//     them. A coin in the blob's notch or its interior hole stays DARK even while the blob sits over it —
//     the collision is exact to the silhouette, not the bounding box (and not the coarse hitbox polygon,
//     which bridges the hole). The borrow reads the sprite's live coverage, so this tracks every flip,
//     rotation, and the transform sway for free.
//
//   • freeze() — an OWNED snapshot. Press F to pin a dim red ghost of the blob where it is now. Coins under
//     the ghost turn RED and STAY red as the live blob wanders off — the owned snapshot keeps answering its
//     captured silhouette after the sprite has moved on. (The optional trail is a ring of these.)
//
//   • approximate() — the polygon forms, drawn as frame Regions hugging the live blob: a Balanced OUTLINE,
//     a Conservative AURA (an inflated halo), and a coarse HITBOX. These are real ShapePoints polygons — the
//     hitbox bridges the hole, which is exactly why the exact contains() above, not the polygon, is what a
//     precise hit test uses.
//
// Motion is cursor-driven and gentle; the sway and trail advance on the sim tick, so it runs the same on
// any display. Keys: F re-freeze the red ghost, O outline, A aura, H hitbox, T trail, B cycle detail
// budget, S hold the pose, Backspace fullscreen; close to quit.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/sprite_shape.h"
#include "retropp/transform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kBlob  = 24;   // blob art side (a whole number of 8px cells)
constexpr int kMapW = 20, kMapH = 18;

enum class Action : std::uint8_t { Freeze, Outline, Aura, Hitbox, Trail, Budget, Spin, Fullscreen };

// A 24×24 lumpy blob: a main body plus a bump, minus a bottom notch (a CONCAVE silhouette) minus a small
// interior HOLE. The concavity makes Conservative vs Balanced differ; the hole makes exact contains()
// visibly differ from the hole-bridging approximate() polygon. Index 1 = body, index 0 = a GameBoy hole.
[[nodiscard]] std::vector<std::uint8_t> blobArt() {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(kBlob) * kBlob, 0);
    for (int y = 0; y < kBlob; ++y)
        for (int x = 0; x < kBlob; ++x) {
            const float md = (x - 11.0f) * (x - 11.0f) + (y - 12.0f) * (y - 12.0f);  // main body
            const float bd = (x - 17.0f) * (x - 17.0f) + (y - 6.0f) * (y - 6.0f);    // bump (top-right)
            const float nd = (x - 11.0f) * (x - 11.0f) + (y - 21.0f) * (y - 21.0f);  // notch (bottom)
            const float hd = (x - 9.0f) * (x - 9.0f) + (y - 11.0f) * (y - 11.0f);    // interior hole
            const bool vis = (md <= 8.5f * 8.5f || bd <= 5.0f * 5.0f) && nd > 5.0f * 5.0f && hd > 2.6f * 2.6f;
            a[static_cast<std::size_t>(y) * kBlob + x] = vis ? 1 : 0;
        }
    return a;
}

// An 8×8 coin disc: index 1 inside, index 0 a GameBoy hole.
[[nodiscard]] std::array<std::uint8_t, 64> coinArt() {
    std::array<std::uint8_t, 64> a{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const float dx = x - 3.5f, dy = y - 3.5f;
            a[static_cast<std::size_t>(y) * 8 + x] = (dx * dx + dy * dy <= 3.5f * 3.5f) ? 1 : 0;
        }
    return a;
}

}  // namespace

int main() {
    SDL_SetMainReady();

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Shape Query Demo"},
        .window   = {.title = "Retro++ — shape-query demo (the sprite is the shape)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};

    const ActionMap map{
        {Action::Freeze, {SDL_SCANCODE_F, PadButton::DpadUp}},
        {Action::Outline, {SDL_SCANCODE_O, PadButton::FaceEast}},
        {Action::Aura, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Hitbox, {SDL_SCANCODE_H, PadButton::FaceWest}},
        {Action::Trail, {SDL_SCANCODE_T, PadButton::FaceNorth}},
        {Action::Budget, {SDL_SCANCODE_B, PadButton::ShoulderR}},
        {Action::Spin, {SDL_SCANCODE_S, PadButton::ShoulderL}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.setActions(map);

    // The blob sheet: uploaded for drawing. That upload is all the shape query needs — a sprite drawn from
    // this sheet answers asShape / freeze / approximate straight off its own `atlas`, with nothing else to
    // hold or pass.
    const std::vector<std::uint8_t> blob = blobArt();
    const AtlasId  blobAtlas = renderer.uploadAtlas(blob.data(), kBlob, kBlob, TransparentIndices::GameBoy);
    const std::array<Rgba8, 2> blobPal{{{0, 0, 0}, {235, 120, 60}}};   // live blob body
    const std::array<Rgba8, 2> ghostPal{{{0, 0, 0}, {150, 55, 55}}};   // frozen art ghost (dim red)
    const PaletteId blobPalId  = renderer.uploadPalette(std::span<const Rgba8>(blobPal));
    const PaletteId ghostPalId = renderer.uploadPalette(std::span<const Rgba8>(ghostPal));

    // A calm two-tone backdrop so the overlays and coins read against content.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 2);
    const AtlasId  sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8);
    const std::array<Rgba8, 2> scenePal{{{28, 32, 44}, {40, 46, 60}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    const std::vector<TileCell> sceneCells(static_cast<std::size_t>(kMapW) * kMapH,
                                           TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId});

    // Coins: static markers. Dark by default; GREEN where the LIVE blob's silhouette covers them; RED where
    // a FROZEN snapshot (or a trail node) covers them and the live blob does not.
    const std::array<std::uint8_t, 64> coin = coinArt();
    const AtlasId coinAtlas = renderer.uploadAtlas(coin.data(), 8, 8, TransparentIndices::GameBoy);
    const std::array<Rgba8, 2> coinDim{{{0, 0, 0}, {90, 84, 40}}};
    const std::array<Rgba8, 2> coinLive{{{0, 0, 0}, {120, 255, 140}}};
    const std::array<Rgba8, 2> coinFrozen{{{0, 0, 0}, {255, 105, 105}}};
    const PaletteId coinDimId    = renderer.uploadPalette(std::span<const Rgba8>(coinDim));
    const PaletteId coinLiveId   = renderer.uploadPalette(std::span<const Rgba8>(coinLive));
    const PaletteId coinFrozenId = renderer.uploadPalette(std::span<const Rgba8>(coinFrozen));
    // A close grid of coins, so the blob's silhouette — its edges, its notch, its hole — sweeps over many.
    std::vector<Vec2i> coins;
    for (int gy = 0; gy < 7; ++gy)
        for (int gx = 0; gx < 9; ++gx) coins.push_back(Vec2i{14 + gx * 16, 14 + gy * 18});

    // The eight flip/rotation orientations the pose cycles through — the shape query re-derives from each.
    struct Orient { Rotation rot; bool flipX; bool flipY; };
    constexpr std::array<Orient, 8> kOrients{{{Rotation::None, false, false},
                                              {Rotation::Rot90, false, false},
                                              {Rotation::Rot180, false, false},
                                              {Rotation::Rot270, false, false},
                                              {Rotation::None, true, false},
                                              {Rotation::None, false, true},
                                              {Rotation::Rot90, true, false},
                                              {Rotation::Rot270, false, true}}};

    // The approximate() point budget the outline / aura cycle through (B). The last is "exact": a budget
    // large enough that nothing is simplified, so approximate() returns the full-resolution outer contour.
    // (Even exact is outer-boundary-only — it bridges the interior hole; the hole shows only in the exact
    // contains() collision, never in an outline.)
    constexpr int kExactBudget = 1 << 14;
    constexpr std::array<int, 5> kBudgets{{6, 10, 16, 32, kExactBudget}};
    constexpr int kCoarseBudget = 5;
    constexpr std::size_t kTrailMax = 14;

    bool  showOutline = true, showAura = false, showHitbox = false, showTrail = false, spin = true;
    int   budgetIdx = 2;
    Vec2i cursor{80, 72};
    bool  cursorOn = true;
    int   heldOrient = 0;
    bool  captureRequested = true;   // capture a frozen snapshot on the first frame

    auto announce = [&]() {
        const int budget = kBudgets[static_cast<std::size_t>(budgetIdx)];
        char detail[16];
        if (budget >= kExactBudget) std::snprintf(detail, sizeof detail, "exact");
        else std::snprintf(detail, sizeof detail, "%d pts", budget);
        std::printf("shape-query — the sprite IS the shape. Coins turn GREEN under the LIVE blob (exact — a "
                    "coin in the notch/hole stays dark), RED under the FROZEN ghost. outline %s, aura %s, "
                    "hitbox %s, trail %s, detail %s, pose %s. F = re-freeze, O/A/H/T toggle, B detail, "
                    "S hold pose, Backspace fullscreen.\n",
                    showOutline ? "ON" : "off", showAura ? "ON" : "off", showHitbox ? "ON" : "off",
                    showTrail ? "ON" : "off", detail, spin ? "cycling" : "held");
    };
    announce();

    loop.setTick([&](const InputState& in) {
        cursor   = in.cursor();
        cursorOn = in.cursorOnScreen();
        if (in.justPressed(Action::Freeze))  { captureRequested = true;    announce(); }
        if (in.justPressed(Action::Outline)) { showOutline = !showOutline; announce(); }
        if (in.justPressed(Action::Aura))    { showAura = !showAura;       announce(); }
        if (in.justPressed(Action::Hitbox))  { showHitbox = !showHitbox;   announce(); }
        if (in.justPressed(Action::Trail))   { showTrail = !showTrail;     announce(); }
        if (in.justPressed(Action::Budget))  { budgetIdx = (budgetIdx + 1) % static_cast<int>(kBudgets.size()); announce(); }
        if (in.justPressed(Action::Spin))    { spin = !spin;               announce(); }
        if (in.justPressed(Action::Fullscreen)) platform.setFullscreen(!platform.isFullscreen());
    });

    FrameDrawState                frame;
    std::vector<Sprite>           sprites;
    std::deque<Sprite>            trailGhosts;
    std::deque<FrozenSpriteShape> trailShapes;
    bool                          haveFrozen = false;
    FrozenSpriteShape             frozen;
    Sprite                        frozenGhost{.key = "frozen-ghost"};

    loop.setRender([&]() {
        const float t = static_cast<float>(loop.tickCount());
        frame.layers.clear();
        frame.regions.clear();
        sprites.clear();

        DrawLayer scene{.key = "scene"};
        scene.z       = 0;
        scene.size    = PixelSize{kViewW, kViewH};
        scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(sceneCells)};
        frame.layers.push_back(scene);

        // The live blob — its own art IS the exact silhouette. Centred on the cursor, pose cycling, a gentle
        // transform sway while `spin` is on.
        const int bx = cursorOn ? cursor.x : 80;
        const int by = cursorOn ? cursor.y : 72;
        if (spin) heldOrient = static_cast<int>(static_cast<long long>(t) / 72 % 8);
        const Orient o = kOrients[static_cast<std::size_t>(heldOrient)];
        const Transform sway =
            spin ? Transform::rotation(20.0f * std::sin(t * 0.02f), 12.0f, 12.0f) : Transform::identity();

        Sprite blob{.key = "blob"};
        blob.x = bx;
        blob.y = by;
        blob.size      = AssetDimensions{kBlob, kBlob};
        blob.atlas     = blobAtlas;
        blob.tile      = 0;
        blob.palette   = blobPalId;
        blob.origin    = {12.0f, 12.0f};
        blob.pivot     = {12.0f, 12.0f};
        blob.rotation  = o.rot;
        blob.flipX     = o.flipX;
        blob.flipY     = o.flipY;
        blob.transform = sway;
        blob.z         = 2;

        // freeze() — capture (or re-capture) the OWNED snapshot: the silhouette AND a copy of the art at its
        // current pose/placement. It never reads the blob again.
        if (captureRequested) {
            frozen              = blob.freeze(Space::Layer);
            frozenGhost         = blob;
            frozenGhost.key     = "frozen-ghost";
            frozenGhost.palette = ghostPalId;
            frozenGhost.alpha   = 0.6f;
            frozenGhost.z       = 1;
            haveFrozen          = true;
            captureRequested    = false;
        }

        // The live borrow — the exact collider for the blob where it is NOW. No materialization: three
        // pointers into the sprite and its sheet.
        const SpriteShape live = blob.asShape(Space::Layer);

        // approximate() → the live polygon overlays, as viewport-space Regions (the blob's layer has no
        // scroll, so Space::Layer answers straight in viewport pixels — where a frame Region composites).
        const int hi = kBudgets[static_cast<std::size_t>(budgetIdx)];
        if (showAura) {
            ShapePoints halo = blob.approximate(hi, Space::Layer, ShapeTrace::Conservative);
            halo.radius = 3.0f;
            frame.regions.push_back(Region{.key     = "aura",
                                           .shape   = halo,
                                           .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                         .fill = Rgba8{80, 200, 255, 255}}},
                                           .alpha   = 0.22f});
        }
        if (showOutline) {
            ShapePoints hug = blob.approximate(hi, Space::Layer, ShapeTrace::Balanced);
            hug.strokeWidth = 1.5f;
            frame.regions.push_back(Region{.key     = "outline",
                                           .shape   = hug,
                                           .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                         .fill = Rgba8{235, 245, 255, 255}}}});
        }
        if (showHitbox) {
            ShapePoints box = blob.approximate(kCoarseBudget, Space::Layer, ShapeTrace::Conservative);
            box.strokeWidth = 1.0f;
            frame.regions.push_back(Region{.key     = "hitbox",
                                           .shape   = box,
                                           .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                                         .fill = Rgba8{255, 190, 60, 255}}},
                                           .alpha   = 0.85f});
        }

        // freeze() as motion history — a ring of owned snapshots, drawn as fading art afterimages, each a
        // live collider. Captured on a slow cadence so the ghosts are legible.
        if (showTrail && cursorOn && static_cast<long long>(t) % 3 == 0) {
            trailGhosts.push_front(blob);
            trailShapes.push_front(blob.freeze(Space::Layer));
            if (trailGhosts.size() > kTrailMax) { trailGhosts.pop_back(); trailShapes.pop_back(); }
        }
        if (!showTrail) { trailGhosts.clear(); trailShapes.clear(); }
        for (std::size_t i = 0; i < trailGhosts.size(); ++i) {
            Sprite g = trailGhosts[i];
            g.key   = "ghost-" + std::to_string(i);
            g.alpha = 0.30f * (1.0f - static_cast<float>(i) / static_cast<float>(kTrailMax));
            g.z     = 0;
            sprites.push_back(g);
        }

        if (haveFrozen) sprites.push_back(frozenGhost);

        // Coins: the exact collision, split by which form covers them. Sampled at the coin's centre and rim,
        // so a coin lights when the silhouette TOUCHES its disc. GREEN = the live borrow covers it (tracks
        // the blob, exact — a coin in the notch or hole stays dark). RED = a frozen snapshot (or trail node)
        // covers it while the live blob does not (the owned form persisting where the sprite has left).
        auto touches = [&](auto&& shape, Vec2i coinPos) {
            const float cx = static_cast<float>(coinPos.x) + 4.0f;
            const float cy = static_cast<float>(coinPos.y) + 4.0f;
            if (shape.contains(Point{cx, cy})) return true;
            for (int k = 0; k < 8; ++k) {
                const float a = 6.2831853f * static_cast<float>(k) / 8.0f;
                if (shape.contains(Point{cx + 3.3f * std::cos(a), cy + 3.3f * std::sin(a)})) return true;
            }
            return false;
        };
        for (std::size_t c = 0; c < coins.size(); ++c) {
            const bool byLive = touches(live, coins[c]);
            bool byFrozen = haveFrozen && touches(frozen, coins[c]);
            for (std::size_t i = 0; !byFrozen && i < trailShapes.size(); ++i) byFrozen = touches(trailShapes[i], coins[c]);
            const PaletteId pal = byLive ? coinLiveId : byFrozen ? coinFrozenId : coinDimId;
            sprites.push_back(Sprite{.key     = "coin-" + std::to_string(c),
                                     .x       = coins[c].x,
                                     .y       = coins[c].y,
                                     .z       = 1,
                                     .size    = AssetDimensions::GameBoy8x8,
                                     .atlas   = coinAtlas,
                                     .tile    = 0,
                                     .palette = pal});
        }

        sprites.push_back(blob);

        DrawLayer actors{.key = "actors"};
        actors.z       = 10;
        actors.size    = PixelSize{kViewW, kViewH};
        actors.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(actors);

        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
