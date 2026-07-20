#include <gtest/gtest.h>

#include "retropp/shader_format.h"

namespace retropp {
namespace {

constexpr unsigned char kSpirvBytes[]    = {0x03, 0x02, 0x23, 0x07};  // SPIR-V magic-ish
constexpr unsigned char kDxilBytes[]     = {0x44, 0x58, 0x42, 0x43};  // 'DXBC' container tag
constexpr unsigned char kMetallibBytes[] = {0x4d, 0x54, 0x4c, 0x42};  // 'MTLB' — metallib is binary, no NUL

constexpr ShaderVariants kAll{
    .spirv    = {kSpirvBytes, sizeof(kSpirvBytes), "main"},
    .dxil     = {kDxilBytes, sizeof(kDxilBytes), "main"},
    .metallib = {kMetallibBytes, sizeof(kMetallibBytes), "main0"},
};

TEST(ShaderFormat, PicksSpirvForVulkanDevice) {
    const auto chosen = selectShader(SDL_GPU_SHADERFORMAT_SPIRV, kAll);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->second, SDL_GPU_SHADERFORMAT_SPIRV);
    EXPECT_EQ(chosen->first.data, kSpirvBytes);
    EXPECT_STREQ(chosen->first.entrypoint, "main");
}

TEST(ShaderFormat, PicksDxilForD3D12Device) {
    const auto chosen = selectShader(SDL_GPU_SHADERFORMAT_DXIL, kAll);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->second, SDL_GPU_SHADERFORMAT_DXIL);
    EXPECT_EQ(chosen->first.data, kDxilBytes);
}

TEST(ShaderFormat, PicksMetallibForMetalDeviceWithRenamedEntrypoint) {
    const auto chosen = selectShader(SDL_GPU_SHADERFORMAT_METALLIB, kAll);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->second, SDL_GPU_SHADERFORMAT_METALLIB);
    EXPECT_EQ(chosen->first.data, kMetallibBytes);
    EXPECT_STREQ(chosen->first.entrypoint, "main0");
}

TEST(ShaderFormat, NoSupportedFormatYieldsNothing) {
    EXPECT_FALSE(selectShader(SDL_GPU_SHADERFORMAT_INVALID, kAll).has_value());
}

TEST(ShaderFormat, MultiFormatMaskResolvesDeterministically) {
    // A device reporting both SPIR-V and DXIL gets SPIR-V (first in preference order).
    const auto chosen = selectShader(
        static_cast<SDL_GPUShaderFormat>(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL),
        kAll);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->second, SDL_GPU_SHADERFORMAT_SPIRV);
}

TEST(ShaderFormat, AbsentVariantFallsThroughToNextSupported) {
    // The device supports SPIR-V and metallib, but no SPIR-V variant was generated — so
    // metallib is chosen rather than handing back empty SPIR-V bytecode.
    constexpr ShaderVariants noSpirv{
        .spirv    = {},
        .dxil     = {kDxilBytes, sizeof(kDxilBytes), "main"},
        .metallib = {kMetallibBytes, sizeof(kMetallibBytes), "main0"},
    };
    const auto chosen = selectShader(
        static_cast<SDL_GPUShaderFormat>(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_METALLIB),
        noSpirv);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->second, SDL_GPU_SHADERFORMAT_METALLIB);
}

TEST(ShaderFormat, SupportedButUngeneratedFormatYieldsNothing) {
    // Device supports only SPIR-V, which we did not generate → nothing (not empty bytes).
    constexpr ShaderVariants metallibOnly{
        .spirv    = {},
        .dxil     = {},
        .metallib = {kMetallibBytes, sizeof(kMetallibBytes), "main0"},
    };
    EXPECT_FALSE(selectShader(SDL_GPU_SHADERFORMAT_SPIRV, metallibOnly).has_value());
}

}  // namespace
}  // namespace retropp
