// Sprite layer fragment shader (indexed atlas + runtime palettes + OBJ transparency + inline effects).
//
// Per output pixel: turn the within-sprite QUAD coordinate into a within-sprite pixel, flip it per the
// sprite's flags, resolve the sprite's own atlas to its region in the flat store via the global
// atlas-region table, address the indexed atlas at the sprite's top-left cell origin + that pixel (a
// w×h sprite reads a contiguous w×h atlas rectangle — a 16×16 sprite spans a 2×2 cell block), Load the
// palette INDEX, DISCARD it if the sheet's transparent-index set marks it a hole (structural
// transparency), Load the colour from the palette store at the sprite's palette offset, DISCARD a
// fully-transparent palette entry (material transparency). Then run the sprite's inline effect run
// (fxOffset/fxCount into the per-frame sprite-effect store): the chain effects transform the pixel colour
// in list order, the region effects grade over it where their quad-space shape covers, and a Transparency
// step scales the pixel's alpha. Finally scale by the layer alpha × the sprite's own alpha. Everything is
// integer Load — no sampler.
//
// A displacing chain effect (RowDisplacement / Ripple) instead moves WHERE the art is read: a pre-pass over
// the sprite's chain records composes the displaced within-sprite coordinate (in the sprite's own art pixels)
// BEFORE the atlas read, and a read that lands off the art is transparent (the default Blank edge) or clamps
// to the border (Stretch). The sprite renders through the analytic branch and its footprint is inflated by
// the displacement bound (retropp::makeGpuSprite) so a displaced crest is never clipped at the static quad.
//
// A Bloom or Glow chain effect sums a 2-D Gaussian neighbourhood of the sprite's own art (whole-art-px taps;
// an off-art tap is transparent) into an aura added over the pixel — the aura writes onto pixels with NO art
// coverage, so an aura-carrying sprite (the has-reach flag, bit 6) skips the out-of-quad and hole discards
// and lets the aura sum decide coverage; its footprint is inflated by the aura radius (spriteRadialReach).
//
// SDL_GPU HLSL conventions (see SDL_CreateGPUShader docs): with no sampled textures, the four read-only
// storage textures take t0..t3 in space2; the uniform buffer is b0 in space3.
//   - t0 space2 : flat ATLAS STORE (R32_UINT; integer Load; all sheets stacked vertically)
//   - t1 space2 : palette store (RGBA8; integer Load; FLAT colours wrapped W wide → texel (flat%W, flat/W))
//   - t2 space2 : global atlas-region table (R32G32B32A32_UINT; texel x = AtlasId → (storeY, cols, transpMaskLo, transpMaskHi))
//   - t3 space2 : per-frame sprite-effect records (R32G32B32A32_FLOAT; ten 16-byte texels per record, one record per
//                 ROW at y = fxOffset + i; a storage TEXTURE, not a storage buffer — the buffer namespace after a
//                 uniform collides with Metal's [[buffer]] indices when the texture and uniform counts differ)
//   - b0 space3 : per-layer fragment uniforms (tile px + layer alpha + palette-store width)
//
// Sprites front-composite by layer z — depth is layer order only. Each sprite names its OWN sheet, so
// one sprite layer mixes sheets: the sheet's region is looked up per-sprite from the global table.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<float4> uPaletteStore : register(t1, space2);
Texture2D<uint4>  uAtlasRegions : register(t2, space2);
// Per-frame sprite-effect records — mirrors retropp::SpriteFxRecord as ten RGBA32F texels per record row:
//   0 head (kind, flags, blend, pointCount as float-valued ints) | 1 gate (alpha, radius, strokeWidth, _) |
//   2 params | 3..5 shape transform inverse rows | 6..9 up to eight quad-space vertices (x0,y0,x1,y1,…).
// A displacing chain step (kind RowDisplacement/Ripple) reuses these lanes: params = (amplitude, frequency,
// phase, axis); a Ripple also reads its centre from gate.yz (art px) and its decay from gate.w. flags bit2
// (kSpriteFxEdgeStretch) picks the out-of-art edge (set = clamp, clear = transparent). A Bloom step's
// params = (radius art px, threshold, intensity, invNorm — the per-axis kernel normalization).
Texture2D<float4> uFxStore      : register(t3, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // register 0: tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colours); flat offset → (f%W, f/W)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
};

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

// The separable blend operator B(d, s) per BlendMode (Normal 0 / Add 1 / Subtract 2 / Multiply 3 /
// Screen 4 / Half 5). Mirrors retropp::blendChannel.
float blendChannel(uint mode, float d, float s) {
    if (mode == 1u) return d + s;
    if (mode == 2u) return d - s;
    if (mode == 3u) return d * s;
    if (mode == 4u) return 1.0f - (1.0f - d) * (1.0f - s);
    if (mode == 5u) return (d + s) * 0.5f;
    return s;  // Normal
}

// Combine src over dst under `mode`, source-alpha-weighted, standard over alpha. Mirrors applyBlendMode.
float4 applyBlendMode(float4 dst, float4 src, uint mode) {
    float sa = src.w;
    float3 b = float3(blendChannel(mode, dst.x, src.x),
                      blendChannel(mode, dst.y, src.y),
                      blendChannel(mode, dst.z, src.z));
    float3 rgb = saturate((1.0f - sa) * dst.xyz + sa * b);
    float a = saturate(sa + dst.w * (1.0f - sa));
    return float4(rgb, a);
}

// Luminance-keyed diagonal sheen boost. Mirrors retropp::applyGleam (same op order, luma weights, 0.6 lift).
float3 applyGleam(float3 c, float u, float v, float4 gp) {
    float d    = u + v * gp.w;               // gp = (sweep, width, gain, slant)
    float ad   = abs(d - gp.x);
    float band = saturate(1.0f - ad / gp.y);
    float crest = band * band;
    float lum  = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float g    = gp.z * crest;
    float lift = lum * g * 0.6f;
    return c * (1.0f + g) + lift;
}

// Cross-channel desaturation — pull each channel toward the pixel's own luminance. Mirrors
// retropp::applySaturation (same op order, luma weights). sat == 1 is a byte-exact identity (amount == 0).
float3 applySaturation(float3 c, float sat) {
    float lum    = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float amount = 1.0f - sat;
    return c - (c - lum) * amount;
}

// Stencil coverage (how far inside, ramped over feather) and survival factor. Mirrors stencilCoverage /
// stencilSurvival. mode: 0 = TransparentInside, 1 = TransparentOutside.
float stencilCoverage(float sd, float feather) {
    if (feather > 0.0f) return saturate(0.5f - sd / feather);
    return sd <= 0.0f ? 1.0f : 0.0f;
}
float stencilSurvival(uint mode, float cov) { return mode == 0u ? (1.0f - cov) : cov; }

// Signed distance from a quad-space point to a record's inline polygon (1 pt = circle, 2 = capsule,
// ≥3 = polygon winding), then radius-inflated + stroke-banded. Mirrors sdPolygon + bandSignedDistance +
// spriteRegionSignedDistance. pointCount 0 ⇒ inside everywhere.
float spriteRegionSignedDistance(float2 p, float2 v[8], uint n, float radius, float stroke) {
    if (n == 0u) return -1e30f;
    float d;
    if (n == 1u) {
        d = length(p - v[0]);
    } else {
        float dd = dot(p - v[0], p - v[0]);
        float s  = 1.0f;
        for (uint i = 0u; i < n; i++) {
            uint   j = (i == 0u) ? (n - 1u) : (i - 1u);
            float2 e = v[j] - v[i];
            float2 w = p - v[i];
            float  ee = dot(e, e);
            float  t  = ee > 0.0f ? clamp(dot(w, e) / ee, 0.0f, 1.0f) : 0.0f;
            float2 bb = w - e * t;
            dd = min(dd, dot(bb, bb));
            if (n >= 3u) {
                bool c1 = p.y >= v[i].y;
                bool c2 = p.y <  v[j].y;
                bool c3 = e.x * w.y > e.y * w.x;
                if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
            }
        }
        d = s * sqrt(dd);
    }
    d = d - radius;
    if (stroke > 0.0f) d = abs(d) - stroke * 0.5f;
    return d;
}

// ── Displacement re-read (mirrors retropp::displaceSourceUv / rippleSourceUv with the sprite's art size as
//    the normalization — a sprite displacement is in the sprite's own art px, and the read snaps to art cells
//    so a row/column shifts as one, crisp) ──────────────────────────────────────────────────
float roundHalfUp(float v) { return floor(v + 0.5f); }

// The centre of the art cell a within-sprite coordinate falls in — the point the wave is evaluated at, so all
// pixels of one art row/column share a displacement (mirrors snapUvToCellCenter with dims = art size).
float2 snapArt(float2 uv, float2 dims) {
    return float2((floor(uv.x * dims.x) + 0.5f) / dims.x, (floor(uv.y * dims.y) + 0.5f) / dims.y);
}

// RowDisplacement: the modulated axis offsets by amplitude·sin(2π(freq·otherAxis + phase)), quantized to whole
// art px. params = (amplitude, frequency, phase, axis: 0 Horizontal / 1 Vertical). amplitude 0 ⇒ identity.
float2 spriteDisplace(float2 uv, float4 params, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float2 e = snapArt(uv, dims);
    if ((uint)params.w == 0u) {  // Horizontal: offset in u, wave over v
        float s   = sin(kTwoPi * (params.y * e.y + params.z));
        float off = roundHalfUp(params.x * s) / dims.x;
        return float2(uv.x + off, uv.y);
    }
    float s   = sin(kTwoPi * (params.y * e.x + params.z));  // Vertical: offset in v, wave over u
    float off = roundHalfUp(params.x * s) / dims.y;
    return float2(uv.x, uv.y + off);
}

// Ripple: a radial re-read pushed along the radius from `center` (art px, in gate.yz) by
// amplitude·sin(2π(freq·dist − phase))·exp(−decay·dist), quantized to whole art px. params = (amplitude,
// frequency, phase, _); gate.w = decay. amplitude 0 or the centre pixel ⇒ identity.
float2 spriteRipple(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float invW = 1.0f / dims.x, invH = 1.0f / dims.y;
    float cu = gate.y * invW, cv = gate.z * invH;   // centre art px → within-sprite uv
    float2 e   = snapArt(uv, dims);
    float  dx  = e.x - cu, dy = e.y - cv;
    float  cx  = dx * (invH / invW);                // aspect-correct so the rings stay circular in art space
    float  dist = sqrt(cx * cx + dy * dy);
    if (dist <= 1e-5f) return uv;                   // the centre has no radial direction
    float  wave   = sin(kTwoPi * (params.y * dist - params.z));
    float  env    = exp(-gate.w * dist);
    float  offset = params.x * wave * env;          // art px
    return float2(uv.x + roundHalfUp(dx / dist * offset) * invW,
                  uv.y + roundHalfUp(dy / dist * offset) * invH);
}

// Swirl: an angular re-read rotating the sample about `center` (art px, in gate.yz) by twist·(1 − t²)²,
// t = dist/radius — the full turn at the centre easing to none at the rim. params = (twist, radius, _, _);
// `twist` arrives already resolved to RADIANS and signed (swirlParams does the degrees conversion), and
// `radius` is in art px. Working space is art px (square units, so the disc stays circular). The evaluation
// point snaps to the art cell centre; the read itself is the exact rotated position (nearest sampling makes
// it crisp). twist 0, radius ≤ 0, or a fragment at or beyond the rim ⇒ identity, its own coordinate exactly.
float2 spriteSwirl(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f || params.y <= 0.0f) return uv;
    float2 e = snapArt(uv, dims) * dims;             // evaluate from the art cell centre, in art px
    float2 c = float2(gate.y, gate.z);
    float2 d = e - c;
    float  r = length(d);
    if (r >= params.y) return uv;                    // outside the disc: its own coordinate
    float  t     = r / params.y;
    float  f     = 1.0f - t * t;
    float  theta = params.x * f * f;
    float  s, cs;
    sincos(theta, s, cs);
    float2 rd = float2(cs * d.x - s * d.y, s * d.x + cs * d.y);
    return (c + rd) / dims;
}

// ── Sprite art read (the transparent-field sample) ─────────────────────────────────────────
//
// The per-fragment sprite context main() publishes so retroppSpriteArtSample — and, in a generated
// sprite-custom variant, a game shader's sampleSource() — can read the sprite's own art with no shader
// inputs threaded through. main() sets these once before the read.
static uint  gSpriteTile;          // top-left atlas cell within the sprite's sheet
static uint  gSpriteAtlasPalette;  // atlas (low 16) | palette flat offset (high 16)
static uint  gSpriteFlags;         // flip / rotation bits (the coverage bits are irrelevant to the read)
static uint  gSpritePackedSize;    // (width << 16) | height
static float gSpriteTilePx;        // tile edge length, px
static float gSpritePaletteW;      // palette-store row width

// Read the sprite's OWN art at a within-sprite quad coordinate `uv` (∈ [0,1]² over the art) — the sprite
// sits in an infinite transparent field, so a read outside the art is transparent (Blank) unless `stretch`
// clamps it to the border texel. Returns straight rgba (the palette colour); a structural hole or a fully
// transparent palette entry returns (0,0,0,0). This is the sprite's material read, shared by main()'s own
// pixel and a custom shader's sampleSource() (the transparent-field domain, one authority).
float4 retroppSpriteArtSample(float2 uv, bool stretch) {
    int2 sz = int2((int)(gSpritePackedSize >> 16), (int)(gSpritePackedSize & 0xFFFFu));
    if ((uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f) && !stretch)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);                 // off-art, Blank → transparent
    int2 px = clamp(int2(floor(uv * float2(sz))), int2(0, 0), sz - int2(1, 1));  // Stretch clamps to the border
    uint flags    = gSpriteFlags;
    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }  // Rot90
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }               // Rot180
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }            // Rot270

    uint atlasId       = gSpriteAtlasPalette & 0xFFFFu;
    uint paletteOffset = gSpriteAtlasPalette >> 16;
    uint4 region       = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeY       = (int)region.x;
    int   atlasCols    = (int)region.y;
    if (atlasCols == 0) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int  tilePx = (int)gSpriteTilePx;
    int  col    = (int)gSpriteTile % atlasCols;
    int  row    = (int)gSpriteTile / atlasCols;
    int2 texel  = int2(col * tilePx + px.x, storeY + row * tilePx + px.y);
    uint idx    = uAtlas.Load(int3(texel, 0));
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;
    if (hole) return float4(0.0f, 0.0f, 0.0f, 0.0f);           // structural transparency
    uint   flat = paletteOffset + idx;
    int    W    = (int)gSpritePaletteW;
    return uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));  // a==0 = material hole
}

// ── Bloom glow (the art-neighbourhood sum) ─────────────────────────────────────────────────
//
// The 2-D Gaussian glow of the sprite's own art around a within-sprite coordinate, in the pipeline's
// premultiplied colour domain: each whole-art-px tap converts to premultiplied, scales by its own
// brightness above the threshold (Rec. 601), and accumulates under the separable kernel
// w(dx)·w(dy), σ = max(radius, 0.5)/2, taps ∈ [−K, K]², K = min(⌈radius⌉, 32). Off-art taps are
// transparent (the sprite's infinite transparent field), so the halo fades past the silhouette edge.
// params = (radius art px, threshold, intensity, invNorm); the result scales by invNorm² (the per-axis
// normalization applied on both axes). The CPU mirror of the pieces is retropp::applyBrightpass /
// gaussianKernelWeight, composed exactly like this loop.
float4 spriteBloomGlow(float2 uv, float4 params, float2 dims) {
    float radius = params.x;
    int   K      = min((int)ceil(radius), 32);
    float sigma  = max(radius, 0.5f) * 0.5f;
    float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float den    = max(1.0f - params.y, 1.0f / 255.0f);
    float4 glow  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop]
    for (int dy = -K; dy <= K; dy++) {
        float wy = exp(-((float)(dy * dy)) * inv2s2);
        [loop]
        for (int dx = -K; dx <= K; dx++) {
            float  w   = wy * exp(-((float)(dx * dx)) * inv2s2);
            float2 tuv = uv + float2((float)dx / dims.x, (float)dy / dims.y);
            float4 s   = retroppSpriteArtSample(tuv, false);
            float4 pm  = float4(s.rgb * s.a, s.a);
            float  lum = pm.r * 0.299f + pm.g * 0.587f + pm.b * 0.114f;
            float  f   = saturate((lum - params.y) / den);
            glow += w * (pm * f);
        }
    }
    return glow * (params.w * params.w);
}

// Add the glow over a straight-rgba running pixel: convert to premultiplied, add the intensity-scaled
// glow, lift the coverage, return straight rgba (mirrors retropp::applyBloomAdd across the straight ↔
// premultiplied boundary). A pixel that gains no glow returns unchanged.
float4 spriteBloomApply(float4 c, float4 glow, float intensity) {
    float3 pmRgb = c.rgb * c.a + intensity * glow.rgb;
    float  a     = saturate(c.a + intensity * glow.a * (1.0f - c.a));
    return float4(a > 0.0f ? pmRgb / a : float3(0.0f, 0.0f, 0.0f), a);
}

// ── Glow aura (the art-neighbourhood scalar-mask sum) ──────────────────────────────────────
//
// Bloom's authored-colour sibling over the same 2-D kernel: each whole-art-px tap contributes a SCALAR
// emission value — its coverage times its straight luminance's survival above the threshold — never its
// colour, so the aura's chroma comes entirely from the authored tint the caller applies. Off-art taps are
// transparent (mask 0), so the aura fades past the silhouette edge; threshold 0 is the whole-coverage
// emission mode (survival 1 — dark art radiates too). params = (radius art px, threshold, intensity,
// invNorm); the result scales by invNorm² (the per-axis normalization applied on both axes). radius ≤ 0 is
// gated to zero — no reach, no aura. The CPU mirror is retropp::glowMask / gaussianKernelWeight, composed
// exactly like this loop; the art is straight rgba, so mask = a·survive(lum) reads its luminance directly
// (equal to glowMask of the premultiplied pixel).
float spriteGlowAura(float2 uv, float4 params, float2 dims) {
    float radius = params.x;
    if (radius <= 0.0f) return 0.0f;
    int   K      = min((int)ceil(radius), 32);
    float sigma  = max(radius, 0.5f) * 0.5f;
    float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float den    = max(1.0f - params.y, 1.0f / 255.0f);
    float m      = 0.0f;
    [loop]
    for (int dy = -K; dy <= K; dy++) {
        float wy = exp(-((float)(dy * dy)) * inv2s2);
        [loop]
        for (int dx = -K; dx <= K; dx++) {
            float  w   = wy * exp(-((float)(dx * dx)) * inv2s2);
            float2 tuv = uv + float2((float)dx / dims.x, (float)dy / dims.y);
            float4 s   = retroppSpriteArtSample(tuv, false);
            float  lum = s.r * 0.299f + s.g * 0.587f + s.b * 0.114f;
            float  f   = params.y <= 0.0f ? 1.0f : saturate((lum - params.y) / den);
            m += w * (s.a * f);
        }
    }
    return m * (params.w * params.w);
}

// Add the tinted aura over a straight-rgba running pixel: convert to premultiplied, add lift·tint, lift the
// coverage, return straight rgba (mirrors retropp::applyGlowAdd across the straight ↔ premultiplied
// boundary). A pixel that gains no aura returns unchanged.
float4 spriteGlowApply(float4 c, float m, float intensity, float3 tint) {
    float  lift  = intensity * m;
    float3 pmRgb = c.rgb * c.a + lift * tint;
    float  a     = saturate(c.a + lift);
    return float4(a > 0.0f ? pmRgb / a : float3(0.0f, 0.0f, 0.0f), a);
}

// ── Custom effect hook ─────────────────────────────────────────────────────────────────────
//
// A Custom chain step runs a game-registered shader inline. The base sprite pipeline carries no game
// shader, so the step is a no-op here (returns the running colour unchanged). A generated sprite-custom
// variant (gen_shader.cmake SPRITE mode) replaces the marker below with the game body + its sampleSource
// (over retroppSpriteArtSample) + a record-lane param loader, and #defines RETROPP_SPRITE_CUSTOM so the
// real retroppSpriteCustom wins. `c` is the running pixel colour, `uv` the within-sprite quad coordinate,
// `ri` the step's record row in the sprite-effect store.
// @retropp:sprite-custom-hook
#ifndef RETROPP_SPRITE_CUSTOM
float4 retroppSpriteCustom(float4 c, float2 uv, int ri) { return c; }
#endif

float4 main(float2 spriteUV : TEXCOORD0,
            nointerpolation uint tile         : TEXCOORD1,
            nointerpolation uint atlasPalette : TEXCOORD2,
            nointerpolation uint flags        : TEXCOORD3,
            nointerpolation uint packedSize   : TEXCOORD4,
            nointerpolation float3 inv0       : TEXCOORD5,
            nointerpolation float3 inv1       : TEXCOORD6,
            nointerpolation float3 inv2       : TEXCOORD7,
            nointerpolation float  spriteAlpha : TEXCOORD8,
            nointerpolation uint fxOffset     : TEXCOORD9,
            nointerpolation uint fxCount      : TEXCOORD10,
            float4 pos : SV_Position) : SV_Target0 {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));  // pixel (width, height)

    float2 fdims = float2(sz);

    // The within-sprite QUAD coordinate (fxUv) — the effect + region-gate space, pre-orientation. A sprite on
    // the analytic flag (bit 4: a transformed sprite on the Viewport grid, or ANY sprite that displaces its own
    // art) resolves coverage per VIEWPORT cell: reconstruct this fragment's viewport-space position from
    // SV_Position (pos.xy is the output-pixel centre; ÷ uComposeScale → viewport space), snap to the cell
    // centre, map that through the screen→unit inverse (perspective divide; behind the projection ⇒ discard).
    // fxUv may land outside the true [0,1)² quad when the footprint is inflated (a displaced crest); the
    // out-of-quad discard happens after the displacement pre-pass. The CPU mirror is retropp::sampleSpriteCell.
    // An untransformed, non-displacing sprite takes the plain source-driven spriteUV path.
    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;      // viewport-cell centre
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;                               // behind the projection
        fxUv = float2((inv0.x * c.x + inv0.y * c.y + inv0.z) / cw,
                      (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw);
    } else {
        fxUv = spriteUV;
    }

    // Displacement pre-pass — compose the chain's displacing effects (RowDisplacement / Ripple) into the
    // within-sprite READ coordinate before the art is read (mirrors retropp::spriteDisplacedRead). readUv ==
    // fxUv for a non-displacing sprite. The last displacing effect's edge governs an out-of-art read. The
    // whole scan is gated by the has-displacement flag (bit 5) so a plain or colour-only sprite never pays for
    // it — its read stays at the plain coordinate.
    float2 readUv = fxUv;
    bool   hasDisp = false;
    uint   dispEdge = 0u;   // 0 = Blank (out-of-art ⇒ transparent), 1 = Stretch (clamp to the border)
    if ((flags & 32u) != 0u) {
        [loop]
        for (uint di = 0u; di < fxCount; di++) {
            int    dri  = int(fxOffset + di);
            float4 dh   = uFxStore.Load(int3(0, dri, 0));   // head: kind, flags
            uint   dk   = (uint)dh.x;
            uint   dfl  = (uint)dh.y;
            if ((dfl & 1u) != 0u) continue;                 // a region step is never a displacing re-read
            if (dk == 1u || dk == 2u || dk == 10u) {        // RowDisplacement / Ripple / Swirl
                float4 dp = uFxStore.Load(int3(2, dri, 0)); // params (amplitude, frequency, phase, axis)
                if (dk == 1u) {
                    readUv = spriteDisplace(readUv, dp, fdims);
                } else if (dk == 2u) {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));  // gate carries the Ripple centre + decay
                    readUv = spriteRipple(readUv, dp, dg, fdims);
                } else {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));  // gate carries the Swirl centre
                    readUv = spriteSwirl(readUv, dp, dg, fdims);
                }
                hasDisp  = true;
                dispEdge = ((dfl & 4u) != 0u) ? 1u : 0u;    // kSpriteFxEdgeStretch
            }
        }
    }

    // A non-displacing, non-blooming analytic sprite covers only its true quad — discard a fragment outside
    // it. A displacing sprite's edge is instead handled by the art read's transparent field below (off-art
    // under Blank ⇒ transparent ⇒ the material discard fires; under Stretch the read clamps to the border);
    // a blooming sprite's halo lives outside the quad, so its coverage is the glow sum's to decide. The
    // plain (non-analytic) path's rasterizer already clamps spriteUV to the quad.
    bool hasReach = (flags & 64u) != 0u;
    if (!hasDisp && !hasReach && analytic) {
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    }

    // Publish the per-fragment sprite context, then read the sprite's own art at the (displaced) coordinate.
    gSpriteTile         = tile;
    gSpriteAtlasPalette = atlasPalette;
    gSpriteFlags        = flags;
    gSpritePackedSize   = packedSize;
    gSpriteTilePx       = uTilePx;
    gSpritePaletteW     = uPaletteStoreW;
    float4 colour = retroppSpriteArtSample(readUv, hasDisp && dispEdge == 1u);
    if (colour.a == 0.0f && !hasReach) discard;  // structural / material transparency: a hole. An aura-carrying
                                                 // sprite keeps the pixel — the aura sum may light it, and
                                                 // a pixel that gains nothing discards on zero final alpha.

    // Inline effect run — the sprite's flattened effects chain then its regions (mirrors
    // retropp::evalSpriteFxRecords). Empty (fxCount 0) is the byte-identical no-effect path. Each record is
    // ten RGBA32F texels on row (fxOffset + i); the head texel's fields are float-valued ints.
    float4 c = colour;   // straight rgba (c.a = palette alpha)
    [loop]
    for (uint i = 0u; i < fxCount; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 gate   = uFxStore.Load(int3(1, ri, 0));   // alpha, radius, strokeWidth, _
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint kind     = (uint)head.x;
        uint flags    = (uint)head.y;
        bool isRegion = (flags & 1u) != 0u;      // kSpriteFxIsRegion
        if (!isRegion) {                          // whole-silhouette chain step
            if (kind == 5u) {                     // ColorFill — replace rgb with the resolved fill
                c.rgb = params.xyz;
            } else if (kind == 6u) {              // Gleam — keyed sheen over the pixel
                c.rgb = applyGleam(c.rgb, fxUv.x, fxUv.y, params);
            } else if (kind == 7u) {              // ColorSaturation — desaturate toward the pixel's luma
                c.rgb = applySaturation(c.rgb, params.x);
            } else if (kind == 4u) {              // Transparency — whole silhouette see-through
                float surv = stencilSurvival((uint)params.x, 1.0f);
                if (surv <= 0.0f) discard;
                c.a *= surv;
            } else if (kind == 8u) {              // Bloom — the art-neighbourhood glow added over the pixel
                float4 glow = spriteBloomGlow(readUv, params, fdims);
                c = spriteBloomApply(c, glow, params.z);
            } else if (kind == 9u) {              // Glow — the authored-colour aura; tint rides the gate lanes
                float m = spriteGlowAura(readUv, params, fdims);
                c = spriteGlowApply(c, m, params.z, float3(gate.y, gate.z, gate.w));
            } else if (kind == 3u) {              // Custom — a game shader inlined by the sprite-custom variant
                c = retroppSpriteCustom(c, fxUv, ri);   // no-op on the base pipeline (returns c)
            }
            continue;                             // None / displacing kinds: no colour transform (displacement
                                                  // already moved the read in the pre-pass above)
        }
        // Region step: gate on the quad-space shape (perspective-correct inverse), ∩ silhouette is implicit.
        float4 iv0 = uFxStore.Load(int3(3, ri, 0));
        float4 iv1 = uFxStore.Load(int3(4, ri, 0));
        float4 iv2 = uFxStore.Load(int3(5, ri, 0));
        float4 p01 = uFxStore.Load(int3(6, ri, 0));
        float4 p23 = uFxStore.Load(int3(7, ri, 0));
        float4 p45 = uFxStore.Load(int3(8, ri, 0));
        float4 p67 = uFxStore.Load(int3(9, ri, 0));
        float2 v[8] = { p01.xy, p01.zw, p23.xy, p23.zw, p45.xy, p45.zw, p67.xy, p67.zw };
        float2 q   = fxUv * float2(sz);
        float  wgt = iv2.x * q.x + iv2.y * q.y + iv2.z;
        float2 loc = float2(iv0.x * q.x + iv0.y * q.y + iv0.z,
                            iv1.x * q.x + iv1.y * q.y + iv1.z) / wgt;
        float  sd  = spriteRegionSignedDistance(loc, v, (uint)head.w, gate.y, gate.z);
        if (kind == 4u) {                         // Transparency — survival everywhere from the SDF
            float cov  = stencilCoverage(sd, params.y);
            c.a *= stencilSurvival((uint)params.x, cov);
            continue;
        }
        bool invert = (flags & 2u) != 0u;         // kSpriteFxInvert
        bool inside = (sd <= 0.0f) != invert;
        if (!inside) continue;
        float4 src = float4(params.xyz, gate.x);       // ColorFill: the fill; gate.x = region alpha
        if (kind == 6u) src = float4(applyGleam(c.rgb, fxUv.x, fxUv.y, params), gate.x);
        else if (kind == 7u) src = float4(applySaturation(c.rgb, params.x), gate.x);
        else if (kind == 8u) {                         // Bloom — the bloomed pixel as the graded source
            float4 bl = spriteBloomApply(c, spriteBloomGlow(readUv, params, fdims), params.z);
            src = float4(bl.rgb, gate.x);
        }
        c = applyBlendMode(c, src, (uint)head.z);      // head.z = region blend
    }
    if (c.a == 0.0f) discard;

    return float4(c.rgb, c.a * uAlpha * spriteAlpha);  // palette α × (effects) × layer α × per-sprite α
}
