// ENG-3.C — the .asm-FILE registration path end to end: point Vm::registerRoutine at a real .asm
// file, the VM reads it, assembles it in-process (no external toolchain), and hands back a typed
// callable. The preset cases in vm_host_test already prove the engine's own routine .asm files
// survive the round-trip byte-for-byte (the dogfood golden); these cases prove the public file form
// for arbitrary consumer routines, plus the error paths (missing file, bad source).
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal paths
#include "retropp/gb.h"
#include "retropp/gb_routines.h"
#include "retropp/vm.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>

#include <gtest/gtest.h>

#ifndef RETROPP_FIXTURES_DIR
#define RETROPP_FIXTURES_DIR "."
#endif

namespace retropp {
namespace {

// The .asm-file path is now the SUGAR door: a compile-time LITERAL path + a policy, resolved against the
// engine's single assetRoot(). The fixtures live in a build-time directory (not a literal), so point
// assetRoot() at it and use LoadFromPath (these fixtures are never baked — no build scan in the test binary).
void useFixtureRoutines() {
    setAssetRoot(std::filesystem::path(RETROPP_FIXTURES_DIR) / "routines");
}

TEST(AsmPipeline, RegisterFromAsmFileAndCall) {
    useFixtureRoutines();
    Vm vm{VMPlatform::GameBoyColor};
    auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
        "add_ab.asm", RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A},
        AssetPolicy::LoadFromPath);
    EXPECT_EQ(add(20, 22), 42);
    EXPECT_EQ(add(200, 55), 255);
}

TEST(AsmPipeline, SixteenBitFromAsmFile) {
    useFixtureRoutines();
    Vm vm{VMPlatform::GameBoyColor};
    auto dbl = vm.registerRoutine<std::uint16_t(std::uint16_t)>(
        "double_hl.asm", RoutineBinding{.inputs = {gb::HL}, .output = gb::HL},
        AssetPolicy::LoadFromPath);
    EXPECT_EQ(dbl(0x1234), 0x2468);
}

TEST(AsmPipeline, MemoryBindingFromAsmFile) {
    useFixtureRoutines();
    Vm vm{VMPlatform::GameBoyColor};
    auto f = vm.registerRoutine<std::uint8_t(std::uint8_t)>(
        "mem_add.asm",
        RoutineBinding{.inputs = {Location::memory(0xFF90)}, .output = Location::memory(0xFF91)},
        AssetPolicy::LoadFromPath);
    EXPECT_EQ(f(0x05), 0x15);
    EXPECT_EQ(f(0x20), 0x30);
}

TEST(AsmPipeline, HardwareSymbolResolvesFromAsmFile) {
    // rDIV resolves through the backend's GB hardware symbol table; two fresh, identically-stepped
    // VMs produce the same first roll (the rDIV read is deterministic on the emulated core).
    useFixtureRoutines();
    auto firstRoll = []() {
        Vm vm{VMPlatform::GameBoyColor};
        auto rng = vm.registerRoutine<std::uint8_t()>("div_read.asm", RoutineBinding{.output = gb::A},
                                                      AssetPolicy::LoadFromPath);
        return rng();
    };
    EXPECT_EQ(firstRoll(), firstRoll());
}

TEST(AsmPipeline, MissingFileThrows) {
    useFixtureRoutines();
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((vm.registerRoutine<std::uint8_t()>("does_not_exist.asm",
                                                     RoutineBinding{.output = gb::A},
                                                     AssetPolicy::LoadFromPath)),
                 std::runtime_error);
}

TEST(AsmPipeline, BadSourceFileThrows) {
    useFixtureRoutines();
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((vm.registerRoutine<std::uint8_t()>("bad_routine.asm",
                                                     RoutineBinding{.output = gb::A},
                                                     AssetPolicy::LoadFromPath)),
                 std::runtime_error);
}

// The engine-owned presets are authored as .asm files (src/vm/gameboy/routines/) and assembled from
// them at registration — assemble + run without throwing. (vm_host_test asserts their exact golden
// streams; this is the file-path smoke that they resolve and execute.)
TEST(AsmPipeline, PresetsAssembleFromTheirAsmFiles) {
    Vm vm{VMPlatform::GameBoyColor};
    auto div = sameboy::divRng(vm);
    auto dual = sameboy::dualSeedRng(vm);
    (void)div();
    (void)dual();
    SUCCEED();
}

}  // namespace
}  // namespace retropp
