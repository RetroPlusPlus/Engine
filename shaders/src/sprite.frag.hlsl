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

    // The within-sprite QUAD coordinate in [0,1) (fxUv) — the effect + region-gate space, pre-orientation.
    // A transformed sprite on the Viewport grid (the analytic flag, bit 4) resolves coverage per VIEWPORT
    // cell: reconstruct this fragment's viewport-space position from SV_Position (pos.xy is the output-pixel
    // centre; ÷ uComposeScale → viewport space), snap to the cell centre, map that through the screen→unit
    // inverse (perspective divide; behind the projection ⇒ discard), and DISCARD when the cell centre falls
    // outside the true [0,1)² quad. The CPU mirror is retropp::sampleSpriteCell. An untransformed sprite
    // (bit off, and every sprite on the Output grid) takes the plain source-driven spriteUV path.
    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;      // viewport-cell centre
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;                               // behind the projection
        float u = (inv0.x * c.x + inv0.y * c.y + inv0.z) / cw;
        float v = (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw;
        if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) discard;  // outside the true quad
        fxUv = float2(u, v);
    } else {
        fxUv = spriteUV;
    }
    int2 px = clamp(int2(floor(fxUv * float2(sz))), int2(0, 0), sz - int2(1, 1));

    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    // Orient the within-sprite pixel: flip first, then the 90° rotation (mirrors retropp::sourceCellTexel
    // with w = sz.x, h = sz.y). A non-square sprite at Rot90/Rot270 transposes the read — permitted.
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }  // Rot90
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }               // Rot180
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }            // Rot270

    uint atlasId       = atlasPalette & 0xFFFFu;       // this sprite's sheet handle
    uint paletteOffset = atlasPalette >> 16;           // this sprite's palette flat offset

    // The sprite's sheet region in the flat store, by its atlas handle: (storeY, cols). An unused/invalid
    // handle reads region 0 → cols 0 → discard (nothing to draw, and never a divide-by-zero).
    uint4 region    = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeY    = (int)region.x;
    int   atlasCols = (int)region.y;
    if (atlasCols == 0) discard;

    int tilePx = (int)uTilePx;
    int col    = (int)tile % atlasCols;
    int row    = (int)tile / atlasCols;
    int2 texel = int2(col * tilePx + px.x, storeY + row * tilePx + px.y);

    uint idx = uAtlas.Load(int3(texel, 0));   // palette index 0..N-1

    // Structural transparency: the sheet's transparent-index set is a 64-bit bitmask split across
    // region.z (indices 0–31) and region.w (indices 32–63). When this index is a member it is a HOLE —
    // discard so the layer below shows through. The empty set (the default) discards nothing.
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;   // indices ≥ 64 are alpha-only, never a structural hole
    if (hole) discard;

    uint   flat   = paletteOffset + idx;   // flat index into the palette store
    int    W      = (int)uPaletteStoreW;
    float4 colour = uPaletteStore.Load(int3((int)(flat % (uint)W), (int)(flat / (uint)W), 0));
    if (colour.a == 0.0f) discard;         // material transparency: a fully-transparent entry is a hole

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
            } else if (kind == 4u) {              // Transparency — whole silhouette see-through
                float surv = stencilSurvival((uint)params.x, 1.0f);
                if (surv <= 0.0f) discard;
                c.a *= surv;
            }
            continue;                             // None / displacing kinds pass through on the v1 sprite path
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
        c = applyBlendMode(c, src, (uint)head.z);      // head.z = region blend
    }
    if (c.a == 0.0f) discard;

    return float4(c.rgb, c.a * uAlpha * spriteAlpha);  // palette α × (effects) × layer α × per-sprite α
}
