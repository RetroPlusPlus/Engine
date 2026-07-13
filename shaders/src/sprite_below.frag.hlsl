// Below-scope sprite fragment shader (scene-facing sprite effects).
//
// The Layer-scope sprite path (sprite.frag) transforms the sprite's OWN art. A Below-scope sprite effect
// instead distorts / grades the COMPOSITED SCENE beneath the sprite's layer, confined to the sprite's
// silhouette (its art alpha coverage). This shader realizes that: the sprite draws as instanced quads (the
// SAME vertex stage as sprite.frag), and per fragment it
//   1. reads the sprite's OWN art at the within-sprite coordinate for the COVERAGE mask (alpha only) —
//      a fragment where the art is transparent is off-silhouette and discards;
//   2. samples the accumulator (the scene, bound as SourceTexture) at the fragment's screen position;
//   3. applies the sprite's Below effect run to that scene sample (built-in kind dispatch, mirroring the
//      retropp:: apply* authorities); a displacing kind re-reads the SCENE at a displaced screen position
//      (amplitude in VIEWPORT px — it distorts the scene, unlike the Layer path's own-art px re-read);
//   4. outputs the graded scene with alpha = the art coverage.
// The renderer renders the layer's Below sprites into a transparent-cleared scratch through this pipeline
// (premultiplied, the stock sprite blend state) and composites that scratch premultiplied-over the
// accumulator — outside the coverage the scratch is transparent, so the scene is byte-identical there. The
// sprite's own art then composites on top via the Layer path (sprite.frag), so a sprite carries both: art
// on top, distorted scene showing through the silhouette. N-flat: every built-in Below sprite in a layer
// draws in ONE instanced pass.
//
// Bindings differ from sprite.frag by ONE sampled texture: the scene (SourceTexture) takes t0/s0 space2, so
// the four storage textures shift to t1..t4 (SDL_GPU numbers sampled textures first, then storage — the
// same convention the layer effect path uses, SourceTexture t0 then RowDataTexture t1).
//   - t0 space2 : the accumulator (scene), SAMPLED (nearest, CLAMP) — the pixels beneath this layer
//   - s0 space2 : its sampler
//   - t1 space2 : flat ATLAS STORE (R32_UINT; integer Load) — for the coverage read
//   - t2 space2 : palette store (RGBA8) — for the coverage read
//   - t3 space2 : global atlas-region table (R32G32B32A32_UINT)
//   - t4 space2 : per-frame sprite-effect records (R32G32B32A32_FLOAT; the sprite's Below run)
//   - b0 space3 : per-layer fragment uniforms (tile px + layer alpha + palette-store width + compose scale)
//
// v1 supports the scene-composing built-in kinds — ColorFill, Gleam, RowDisplacement, Ripple. Transparency
// at Below scope (punch the whole scene see-through to the backdrop within the silhouette) needs a distinct
// alpha-through composite and is deferred (the renderer warns); a region-confined Below effect is deferred
// too (whole-silhouette only).

Texture2D<float4> SourceTexture : register(t0, space2);   // the scene beneath (nearest, CLAMP)
SamplerState      SourceSampler : register(s0, space2);
Texture2D<uint>   uAtlas        : register(t1, space2);
Texture2D<float4> uPaletteStore : register(t2, space2);
Texture2D<uint4>  uAtlasRegions : register(t3, space2);
Texture2D<float4> uFxStore      : register(t4, space2);   // the sprite's Below effect records

cbuffer SpriteFragUniforms : register(b0, space3) {
    float uTilePx;          // tile edge length, pixels (8)
    float uAlpha;           // layer alpha, [0,1]
    float uPaletteStoreW;   // palette-store row width (colours)
    float uComposeScale;    // compose grid ÷ viewport (1 = faithful); output pixel → viewport
};

// ── Effect math (mirrors the retropp:: CPU authorities in postprocess.h) ──────────────────

// Luminance-keyed diagonal sheen boost. Mirrors retropp::applyGleam (same op order, luma weights, 0.6 lift).
float3 applyGleam(float3 c, float u, float v, float4 gp) {
    float d     = u + v * gp.w;               // gp = (sweep, width, gain, slant)
    float ad    = abs(d - gp.x);
    float band  = saturate(1.0f - ad / gp.y);
    float crest = band * band;
    float lum   = c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
    float g     = gp.z * crest;
    float lift  = lum * g * 0.6f;
    return c * (1.0f + g) + lift;
}

// Round-half-up (HLSL round() is round-to-even and breaks scale-1 parity at .5 ties).
float roundHalfUp(float v) { return floor(v + 0.5f); }

// The centre of the VIEWPORT cell a screen uv falls in — the crisp evaluation point (the scene is a
// viewport-resolution image scaled to compose res; per-cell math matches the viewport rasterization).
float2 snapViewport(float2 uv, float2 dims) {
    float2 c = uv;
    if (dims.x > 0.0f) c.x = (floor(uv.x * dims.x) + 0.5f) / dims.x;
    if (dims.y > 0.0f) c.y = (floor(uv.y * dims.y) + 0.5f) / dims.y;
    return c;
}

// RowDisplacement over the SCENE: the modulated axis offsets by amplitude·sin(2π(freq·otherAxis + phase)),
// quantized to whole VIEWPORT px (the scene's native pixels). params = (amplitude, frequency, phase, axis:
// 0 Horizontal / 1 Vertical). amplitude 0 ⇒ identity. `dims` is the viewport size.
float2 sceneDisplace(float2 uv, float4 params, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float2 e = snapViewport(uv, dims);
    if ((uint)params.w == 0u) {  // Horizontal: offset in u, wave over v
        float s   = sin(kTwoPi * (params.y * e.y + params.z));
        float off = roundHalfUp(params.x * s) / dims.x;
        return float2(uv.x + off, uv.y);
    }
    float s   = sin(kTwoPi * (params.y * e.x + params.z));  // Vertical: offset in v, wave over u
    float off = roundHalfUp(params.x * s) / dims.y;
    return float2(uv.x, uv.y + off);
}

// Ripple over the SCENE: a radial re-read pushed along the radius from `center` (VIEWPORT px, in gate.yz)
// by amplitude·sin(2π(freq·dist − phase))·exp(−decay·dist), quantized to whole viewport px. params =
// (amplitude, frequency, phase, _); gate.w = decay. amplitude 0 or the centre pixel ⇒ identity.
float2 sceneRipple(float2 uv, float4 params, float4 gate, float2 dims) {
    if (params.x == 0.0f) return uv;
    const float kTwoPi = 6.283185307179586f;
    float invW = 1.0f / dims.x, invH = 1.0f / dims.y;
    float cu = gate.y * invW, cv = gate.z * invH;   // centre viewport px → screen uv
    float2 e   = snapViewport(uv, dims);
    float  dx  = e.x - cu, dy = e.y - cv;
    float  cx  = dx * (invH / invW);                // aspect-correct so the rings stay circular
    float  dist = sqrt(cx * cx + dy * dy);
    if (dist <= 1e-5f) return uv;                   // the centre has no radial direction
    float  wave   = sin(kTwoPi * (params.y * dist - params.z));
    float  env    = exp(-gate.w * dist);
    float  offset = params.x * wave * env;          // viewport px
    return float2(uv.x + roundHalfUp(dx / dist * offset) * invW,
                  uv.y + roundHalfUp(dy / dist * offset) * invH);
}

// ── Sprite art coverage read (the silhouette mask) ─────────────────────────────────────────
//
// Read the sprite's OWN art alpha at a within-sprite quad coordinate to decide coverage — the same
// transparent-field material read sprite.frag does, but only the alpha matters here (the mask). A read
// outside the art, a structural hole, or a fully-transparent palette entry is coverage 0 (off-silhouette).
// (Duplicated from sprite.frag rather than shared, to leave the shipped, golden sprite path untouched; a
// future cleanup can factor the read into a shared include.)
float4 spriteArtSample(float2 uv, uint tile, uint atlasPalette, uint flags, uint packedSize) {
    int2 sz = int2((int)(packedSize >> 16), (int)(packedSize & 0xFFFFu));
    if (uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);                 // off-art → no coverage
    int2 px = clamp(int2(floor(uv * float2(sz))), int2(0, 0), sz - int2(1, 1));
    bool flipX    = (flags & 1u) != 0u;
    bool flipY    = (flags & 2u) != 0u;
    uint rotation = (flags >> 2u) & 3u;
    if (flipX) px.x = sz.x - 1 - px.x;
    if (flipY) px.y = sz.y - 1 - px.y;
    if (rotation == 1u)      { int rt = px.x; px.x = px.y;            px.y = sz.x - 1 - rt; }
    else if (rotation == 2u) { px.x = sz.x - 1 - px.x; px.y = sz.y - 1 - px.y; }
    else if (rotation == 3u) { int rt = px.x; px.x = sz.y - 1 - px.y;  px.y = rt; }

    uint atlasId       = atlasPalette & 0xFFFFu;
    uint paletteOffset = atlasPalette >> 16;
    uint4 region       = uAtlasRegions.Load(int3((int)atlasId, 0, 0));
    int   storeY       = (int)region.x;
    int   atlasCols    = (int)region.y;
    if (atlasCols == 0) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int  tilePx = (int)uTilePx;
    int  col    = (int)tile % atlasCols;
    int  row    = (int)tile / atlasCols;
    int2 texel  = int2(col * tilePx + px.x, storeY + row * tilePx + px.y);
    uint idx    = uAtlas.Load(int3(texel, 0));
    bool hole = (idx < 32u) ? (((region.z >> idx)         & 1u) != 0u)
              : (idx < 64u) ? (((region.w >> (idx - 32u)) & 1u) != 0u)
                            : false;
    if (hole) return float4(0.0f, 0.0f, 0.0f, 0.0f);           // structural transparency
    uint   flat = paletteOffset + idx;
    int    W    = (int)uPaletteStoreW;
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
    // The within-sprite QUAD coordinate — the coverage-read space, pre-orientation. A transformed sprite on
    // the analytic flag (bit 4) resolves coverage per VIEWPORT cell (SV_Position → viewport → cell centre →
    // screen→unit inverse); a plain sprite takes the rasterized spriteUV. A Below sprite's footprint is NOT
    // inflated (Below displacement re-reads the scene, not the art — spriteDisplacedRead skips Below), so a
    // fragment outside the true quad on the analytic path is off-silhouette and discards.
    bool analytic = (flags & 16u) != 0u;
    float2 fxUv;
    if (analytic) {
        float2 c  = floor(pos.xy / uComposeScale) + 0.5f;
        float  cw = inv2.x * c.x + inv2.y * c.y + inv2.z;
        if (cw <= 0.0f) discard;
        fxUv = float2((inv0.x * c.x + inv0.y * c.y + inv0.z) / cw,
                      (inv1.x * c.x + inv1.y * c.y + inv1.z) / cw);
        if (fxUv.x < 0.0f || fxUv.x >= 1.0f || fxUv.y < 0.0f || fxUv.y >= 1.0f) discard;
    } else {
        fxUv = spriteUV;
    }

    // Coverage: the sprite's own art alpha. Off-silhouette (transparent art) ⇒ nothing to distort here.
    float coverage = spriteArtSample(fxUv, tile, atlasPalette, flags, packedSize).a;
    if (coverage == 0.0f) discard;

    // The scene sample coordinate: this fragment's screen position over the compose target (the accumulator
    // is the compose-resolution scene). Its viewport size drives the displacement math (viewport px).
    float2 composeDim;
    SourceTexture.GetDimensions(composeDim.x, composeDim.y);
    float2 sceneUv     = pos.xy / composeDim;
    float2 viewportDim = composeDim / uComposeScale;

    // Displacement pre-pass — compose the Below run's displacing effects (RowDisplacement / Ripple) into the
    // scene READ coordinate before the scene is sampled. Colour kinds then apply to the read colour.
    float2 readUv = sceneUv;
    [loop]
    for (uint di = 0u; di < fxCount; di++) {
        int    dri = int(fxOffset + di);
        float4 dh  = uFxStore.Load(int3(0, dri, 0));   // head: kind, flags
        uint   dk  = (uint)dh.x;
        if (dk == 1u) {                                 // RowDisplacement
            float4 dp = uFxStore.Load(int3(2, dri, 0)); // params (amplitude, frequency, phase, axis)
            readUv = sceneDisplace(readUv, dp, viewportDim);
        } else if (dk == 2u) {                          // Ripple
            float4 dp = uFxStore.Load(int3(2, dri, 0));
            float4 dg = uFxStore.Load(int3(1, dri, 0)); // gate: centre (yz) + decay (w)
            readUv = sceneRipple(readUv, dp, dg, viewportDim);
        }
    }

    // Sample the scene at the (displaced) coordinate — nearest, CLAMP at the frame edge (a displaced read
    // that runs off-frame smears the border rather than punching a transparent gap into the scene).
    float2 clampedUv = clamp(readUv, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
    float3 c = SourceTexture.Sample(SourceSampler, clampedUv).rgb;

    // Colour kinds — apply to the scene sample in chain order (mirrors evalSpriteFxRecords for the
    // scene-facing kinds). ColorFill replaces, Gleam adds a keyed sheen; displacing kinds already moved the
    // read above; Transparency / Custom at Below scope are not on the v1 path (the renderer excludes them).
    [loop]
    for (uint i = 0u; i < fxCount; i++) {
        int    ri     = int(fxOffset + i);
        float4 head   = uFxStore.Load(int3(0, ri, 0));   // kind, flags, blend, pointCount
        float4 params = uFxStore.Load(int3(2, ri, 0));
        uint   kind   = (uint)head.x;
        if (kind == 5u)      c   = params.xyz;                                  // ColorFill — paint over the scene
        else if (kind == 6u) c   = applyGleam(c, sceneUv.x, sceneUv.y, params); // Gleam — keyed sheen over the scene
    }

    // Output the graded scene, opacity = coverage × layer α × per-sprite α. The renderer composites this
    // (premultiplied by the stock sprite blend state) premultiplied-over the accumulator, so the distortion
    // lands only on the silhouette; the transparent surround leaves the scene byte-identical.
    return float4(c, coverage * uAlpha * spriteAlpha);
}
