#include "assets.h"

#include <chrono>

#include "retropp/asset_policy.h"  // AssetPolicy (Embed)
#include "retropp/geometry.h"      // AssetDimensions

namespace ferryman {

using namespace retropp;
using namespace std::chrono_literals;

FerrymanAssets loadFerrymanAssets(Renderer& renderer) {
    FerrymanAssets a;

    // ── The three committed indexed PNGs. ALL Embed (baked into the binary by the build scan) —
    //    the policy is decided HERE, per loadAtlas call, as a literal token; no build rule.
    //    The sprite sheet's index 0 is the structural hole (the GameBoy convention); the terrain
    //    sheet holes index 0 too (the water planes show through the blank tile and the sparkle
    //    overlays' empty pixels); the font's holes come from palette ALPHA (the floating set).
    //    Each sheet is a standard 8-wide grid, so the trailing cells of the last row are unused
    //    padding — the `count` trims the manifest to exactly the real assets.
    a.font    = renderer.loadAtlas("examples/ferryman/assets/ferryman_font.png",
                                   AssetDimensions{16, 16}, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown, /*count=*/38,
                                   TransparentIndices::None,
                                   /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.terrain = renderer.loadAtlas("examples/ferryman/assets/ferryman_terrain.png",
                                   AssetDimensions{32, 32}, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown, /*count=*/68,
                                   TransparentIndices::of({0}),
                                   /*framesPerAnimation=*/0, AssetPolicy::Embed);
    a.sheet   = renderer.loadAtlas("examples/ferryman/assets/ferryman_sprites.png",
                                   AssetDimensions{48, 48}, ContentKind::SpriteSeries,
                                   ReadOrder::LeftRightThenDown, /*count=*/23,
                                   TransparentIndices::GameBoy,
                                   /*framesPerAnimation=*/0, AssetPolicy::Embed);
    //    The bespoke title glyphs float on palette alpha (entry 0 is an alpha-0 material hole).
    a.title   = renderer.loadAtlas("examples/ferryman/assets/ferryman_title.png",
                                   AssetDimensions{32, 32}, ContentKind::SpriteSeries,
                                   ReadOrder::LeftRightThenDown, /*count=*/8,
                                   TransparentIndices::None,
                                   /*framesPerAnimation=*/0, AssetPolicy::Embed);

    // ── Every palette from a PALETTE IMAGE: a 16×1 RGBA PNG, one pixel per entry, loaded with
    //    loadPaletteImage (its per-type default policy is Embed — bespoke build-time colour
    //    data). Alpha rides in the image: the floating-text palettes carry an alpha-0 entry 0.
    //    Sprite palettes, in Pal-enum order:
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/ferry.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/colonist_a.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/colonist_b.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/colonist_c.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/dart_a.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/dart_b.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/sweeper_a.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/sweeper_b.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/hauler_a.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/hauler_b.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/abductor.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/mutant.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/boom.png"));
    //    The two bolt liveries — same art, two allegiances: hostile fire reads hot magenta, the
    //    crew's return fire reads gold. These MUST follow boom so spritePals lines up with the
    //    Pal enum (PAL_BOLT_ENEMY = 13, PAL_BOLT_CARGO = 14); render.cpp indexes them by that enum.
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/bolt_enemy.png"));
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/bolt_cargo.png"));
    //    The flat shadow silhouette (flying craft redraw their art through it, PAL_SHADOW = 15):
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/shadow.png"));
    //    The boat's wake foam (PAL_WAKE = 16):
    a.spritePals.push_back(renderer.loadPaletteImage("examples/ferryman/assets/palettes/wake.png"));
    //    Terrain palettes, in TerrainPal order:
    a.terrainPals[TP_WATER_A]  = renderer.loadPaletteImage("examples/ferryman/assets/palettes/water_a.png");
    a.terrainPals[TP_WATER_B]  = renderer.loadPaletteImage("examples/ferryman/assets/palettes/water_b.png");
    a.terrainPals[TP_SHORE]    = renderer.loadPaletteImage("examples/ferryman/assets/palettes/shore.png");
    a.terrainPals[TP_LANE]     = renderer.loadPaletteImage("examples/ferryman/assets/palettes/lane.png");
    a.terrainPals[TP_MEDIAN]   = renderer.loadPaletteImage("examples/ferryman/assets/palettes/median.png");
    a.terrainPals[TP_SANCTUARY] = renderer.loadPaletteImage("examples/ferryman/assets/palettes/sanctuary.png");
    a.terrainPals[TP_BEACON_A] = renderer.loadPaletteImage("examples/ferryman/assets/palettes/beacon_a.png");
    a.terrainPals[TP_BEACON_B] = renderer.loadPaletteImage("examples/ferryman/assets/palettes/beacon_b.png");
    //    The font's dual liveries (floating alpha-0-backed vs the opaque HUD bar):
    a.textPals[TXT_WHITE] = renderer.loadPaletteImage("examples/ferryman/assets/palettes/text_white.png");
    a.textPals[TXT_GOLD]  = renderer.loadPaletteImage("examples/ferryman/assets/palettes/text_gold.png");
    a.textPals[TXT_CYAN]  = renderer.loadPaletteImage("examples/ferryman/assets/palettes/text_cyan.png");
    a.hudPals[TXT_WHITE]  = renderer.loadPaletteImage("examples/ferryman/assets/palettes/hud_white.png");
    a.hudPals[TXT_GOLD]   = renderer.loadPaletteImage("examples/ferryman/assets/palettes/hud_gold.png");
    a.hudPals[TXT_CYAN]   = renderer.loadPaletteImage("examples/ferryman/assets/palettes/hud_cyan.png");
    a.titlePal            = renderer.loadPaletteImage("examples/ferryman/assets/palettes/title.png");

    // The reality-warp custom shader stage (registered by literal path — the build scan compiles it
    // through the per-platform toolchain and reflects its params onto ScreenSpaceEffect). Shared by
    // the ferry and the mutant; warpChroma at each call site picks plain-distortion vs psychedelic.
    a.wakeWarp = renderer.registerPostProcessStage("examples/ferryman/shaders/wake_warp.frag.hlsl");

    // ── The shared clips: every animation idiom, live at once. Frame clips change the ART; the
    //    beacon + shimmer clips change only the PALETTE; the lights clip is a 2-beat metronome
    //    whose frame INDEX picks each livery's light phase.
    a.boomClip = Animation{{
        a.sheet.frame(S_BOOM_0, a.spritePals[PAL_BOOM], 90ms, "flash"),
        a.sheet.frame(S_BOOM_1, a.spritePals[PAL_BOOM], 100ms, "blast"),
        a.sheet.frame(S_BOOM_2, a.spritePals[PAL_BOOM], 120ms, "embers"),
    }};
    a.thrusterClip = Animation{{
        a.sheet.frame(S_FERRY_A, a.spritePals[PAL_FERRY], 120ms, "podA"),
        a.sheet.frame(S_FERRY_B, a.spritePals[PAL_FERRY], 120ms, "podB"),
    }};
    for (int look = 0; look < kColonistLooks; ++look) {
        const auto slotA = static_cast<Slot>(S_COL_HOOD_A + look * 2);
        const auto slotB = static_cast<Slot>(S_COL_HOOD_A + look * 2 + 1);
        const PaletteId pal =
            a.spritePals[static_cast<std::size_t>(PAL_COLONIST_A) + static_cast<std::size_t>(look)];
        a.bobClips[static_cast<std::size_t>(look)] = Animation{{
            a.sheet.frame(static_cast<std::size_t>(slotA), pal, 350ms, "rise"),
            a.sheet.frame(static_cast<std::size_t>(slotB), pal, 350ms, "dip"),
        }};
    }
    a.wingsClip = Animation{{
        a.sheet.frame(S_ABDUCTOR_A, a.spritePals[PAL_ABDUCTOR], 200ms, "lightsA"),
        a.sheet.frame(S_ABDUCTOR_B, a.spritePals[PAL_ABDUCTOR], 200ms, "lightsB"),
    }};
    a.pulseClip = Animation{{
        a.sheet.frame(S_MUTANT_A, a.spritePals[PAL_MUTANT], 160ms, "swellA"),
        a.sheet.frame(S_MUTANT_B, a.spritePals[PAL_MUTANT], 160ms, "swellB"),
    }};
    a.beaconClip = Animation{{  // palette animation: the gold heart breathes, the art holds
        a.terrain.frame(T_BEACON, a.terrainPals[TP_BEACON_A], 600ms, "glowA"),
        a.terrain.frame(T_BEACON, a.terrainPals[TP_BEACON_B], 600ms, "glowB"),
    }};
    a.waterClip = Animation{{  // the sea ROLLS: frame + palette animation at once — its index
        a.terrain.frame(T_WATER_0, a.terrainPals[TP_WATER_A], 400ms, "roll0"),      // picks every
        a.terrain.frame(T_WATER_0 + 1, a.terrainPals[TP_WATER_B], 400ms, "roll1"),  // variant's
        a.terrain.frame(T_WATER_0 + 2, a.terrainPals[TP_WATER_A], 400ms, "roll2"),  // phase slot
    }};
    a.lightsClip = Animation{{  // the running-light metronome (its INDEX is what's read)
        a.sheet.frame(S_DART, a.spritePals[PAL_DART_A], 260ms, "phase0"),
        a.sheet.frame(S_DART, a.spritePals[PAL_DART_B], 260ms, "phase1"),
    }};

    return a;
}

}  // namespace ferryman
