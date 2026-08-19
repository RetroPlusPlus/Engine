// Shader-registry linkage probe. Registered by path from the static-library fixture
// (tests/registry_linkage/) and never drawn: the test asserts the generated shader registry survives the
// link out of an archive, so what matters is that this path resolves to compiled variants at runtime.
// Parameterless, so it needs no cbuffer and no packer beyond the one the build generates for every stage.

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return sampleSource(uv);
}
