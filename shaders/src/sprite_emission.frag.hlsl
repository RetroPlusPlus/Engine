// Sprite emission fragment — the raster stage of a sprite's Layer-scope Bloom / Glow.
//
// A glowing sprite draws TWICE: once through sprite.frag for its art, and once through this shader for the
// light it radiates. This pass writes only the EMISSION — the halo's source field — into the shared
// emission buffer, additively, so every sprite in the same (kind, reach) bucket accumulates into one image
// that is blurred once and composited once. That is what makes the cost of a field of glowing sprites the
// cost of its distinct reaches rather than of its sprite count.
//
// The sprite's own chain runs here up to the emission step, which is named by bits 8..15 of the record's
// flags word (retropp::kSpriteEmissionIndexShift; the renderer ORs it into the emission copy of the art
// record). Colour steps before it transform the pixel exactly as they do for the art, so a recoloured
// sprite radiates its new colour; the step at that index resolves the emission from the running colour:
//
//   Bloom:  e = premultiply(c) · f(lum(premultiply(c)), threshold) · intensity     // its own light
//   Glow:   m = c.a · survive(lum(c), threshold);  e = (tint · m · intensity, m · intensity)
//
// mirroring retropp::emissionExtractBloom / emissionExtractGlow with the sprite's art as the source. The
// result scales by the layer alpha and the per-sprite alpha, so a fading sprite's halo fades with it.
//
// A Custom chain step is a no-op here — a game shader has no emission variant, so the emission sees the
// colour as it stood before that step (the renderer warns). Region steps never precede a chain step in the
// record slice, and are skipped defensively.
//
// The displacement pre-pass, the art read, and the transparent-field semantics are the art shader's, quoted
// verbatim: the halo radiates from wherever the art actually landed. There is no footprint inflation — this
// pass covers the art's own quad and the blur that follows spreads the light in buffer space.
//
// SDL_GPU HLSL conventions match sprite.frag exactly (t0..t3 space2 storage textures, b0 space3 uniform),
// so the two share one vertex stage and one set of bindings.

Texture2D<uint>   uAtlas        : register(t0, space2);
Texture2D<float4> uPaletteStore : register(t1, space2);
Texture2D<uint4>  uAtlasRegions : register(t2, space2);
Texture2D<float4> uFxStore      : register(t3, space2);

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colours); flat offset → (f%W, f/W)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
};

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

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

float3 applySaturation(float3 c, float sat) {
    float lum    = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float amount = 1.0f - sat;
    return c - (c - lum) * amount;
}

// ── Displacement re-read (identical to sprite.frag — the halo follows the displaced art) ──

float roundHalfUp(float v) { return floor(v + 0.5f); }

float2 snapArt(float2 uv, float2 dims) {
    return float2((floor(uv.x * dims.x) + 0.5f) / dims.x, (floor(uv.y * dims.y) + 0.5f) / dims.y);
}

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

// ── Sprite art read (identical to sprite.frag's transparent-field sample) ─────────────────

static uint  gSpriteTile;
static uint  gSpriteAtlasPalette;
static uint  gSpriteFlags;
static uint  gSpritePackedSize;
static float gSpriteTilePx;
static float gSpritePaletteW;

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
    int2   sz    = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));
    float2 fdims = float2(sz);

    // Which record carries the emission — the renderer's index, in the flags word's high byte.
    uint emitIndex = (flags >> 8u) & 0xFFu;
    if (emitIndex >= fxCount) discard;   // no emission step on this record: nothing to radiate

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

    // Displacement pre-pass over the WHOLE chain, exactly as the art pass runs it, so the halo radiates
    // from where the art landed rather than from where it was authored.
    float2 readUv   = fxUv;
    bool   hasDisp  = false;
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
                float4 dp = uFxStore.Load(int3(2, dri, 0));
                if (dk == 1u) {
                    readUv = spriteDisplace(readUv, dp, fdims);
                } else if (dk == 2u) {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));
                    readUv = spriteRipple(readUv, dp, dg, fdims);
                } else {
                    float4 dg = uFxStore.Load(int3(1, dri, 0));
                    readUv = spriteSwirl(readUv, dp, dg, fdims);
                }
                hasDisp  = true;
                dispEdge = ((dfl & 4u) != 0u) ? 1u : 0u;    // kSpriteFxEdgeStretch
            }
        }
    }

    if (!hasDisp && analytic) {
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    }

    gSpriteTile         = tile;
    gSpriteAtlasPalette = atlasPalette;
    gSpriteFlags        = flags;
    gSpritePackedSize   = packedSize;
    gSpriteTilePx       = uTilePx;
    gSpritePaletteW     = uPaletteStoreW;
    float4 c = retroppSpriteArtSample(readUv, hasDisp && dispEdge == 1u);
    if (c.a == 0.0f) discard;            // an uncovered pixel radiates nothing

    // Colour steps BEFORE the emission step, in chain order — what the sprite looks like is what it
    // radiates. Kinds with no colour transform (displacement, another glow) pass through.
    [loop]
    for (uint i = 0u; i < emitIndex; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint   kind   = (uint)head.x;
        if (((uint)head.y & 1u) != 0u) continue;         // region steps never precede a chain step
        if (kind == 5u)      c.rgb = params.xyz;                                     // ColorFill
        else if (kind == 6u) c.rgb = applyGleam(c.rgb, fxUv.x, fxUv.y, params);      // Gleam
        else if (kind == 7u) c.rgb = applySaturation(c.rgb, params.x);               // ColorSaturation
        else if (kind == 4u) {                                                        // Transparency
            float surv = (uint)params.x == 0u ? 0.0f : 1.0f;   // whole silhouette: coverage is 1 everywhere
            if (surv <= 0.0f) discard;
            c.a *= surv;
        }
    }

    // The emission step itself.
    int    eri    = int(fxOffset + emitIndex);
    float4 ehead  = uFxStore.Load(int3(0, eri, 0));
    float4 egate  = uFxStore.Load(int3(1, eri, 0));   // Glow: the authored tint on the idle chain lanes
    float4 eparam = uFxStore.Load(int3(2, eri, 0));   // (radius art px, threshold, intensity, invNorm)
    float  threshold = eparam.y;
    float  intensity = eparam.z;
    float  scale     = uAlpha * spriteAlpha;          // the sprite's light fades as the sprite does

    if ((uint)ehead.x == 9u) {                        // Glow — a scalar mask times the authored tint
        float lum  = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
        float den  = max(1.0f - threshold, 1.0f / 255.0f);
        float surv = threshold <= 0.0f ? 1.0f : saturate((lum - threshold) / den);
        float m    = c.a * surv * intensity * scale;
        return float4(egate.y * m, egate.z * m, egate.w * m, m);
    }

    float4 pm  = float4(c.rgb * c.a, c.a);            // Bloom — the brightpass of its own light
    float  lum = pm.r * 0.299f + pm.g * 0.587f + pm.b * 0.114f;
    float  den = max(1.0f - threshold, 1.0f / 255.0f);
    float  f   = saturate((lum - threshold) / den);
    return pm * (f * intensity * scale);
}
