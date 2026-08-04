// Below-scope emission consumer WITHOUT a body — the demand fills through the stock brightpass at `.threshold`,
// so a one-declaration-line below lens still gets a scene halo (of the scene's own luminance-keyed light). The
// low-luminance blue of the device scene falls below the brightpass floor, so this sibling glows nothing from
// it — the mirror of the body probe.
//
// @retropp:emission

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv) + sampleEmission(uv);   // the scene beneath plus its (luminance-keyed) halo
}
