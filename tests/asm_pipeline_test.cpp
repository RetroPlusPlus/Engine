// ENG-3.C — the .asm-FILE registration path end to end: point Vm::registerRoutine at a real .asm
// file, the VM reads it, assembles it in-process (no external toolchain), and hands back a typed
// callable. The preset cases in vm_host_test already prove the engine's own routine .asm files
// survive the round-trip byte-for-byte (the dogfood golden); these cases prove the public file form
// for arbitrary consumer routines, plus the error paths (missing file, bad source).
#include "retropp/gb.h"
#include "retropp/gb_routines.h"
#include "retropp/vm.h"

#include <cstdint>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#ifndef RETROPP_FIXTURES_DIR
#define RETROPP_FIXTURES_DIR "."
#endif

namespace retropp {
namespace {

std::string routine(const char* name) {
    return std::string(RETROPP_FIXTURES_DIR) + "/routines/" + name;
}

TEST(AsmPipeline, RegisterFromAsmFileAndCall) {
    Vm vm{VMPlatform::GameBoyColor};
    auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
        routine("add_ab.asm"), RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A});
    EXPECT_EQ(add(20, 22), 42);
    EXPECT_EQ(add(200, 55), 255);
}

TEST(AsmPipeline, SixteenBitFromAsmFile) {
    Vm vm{VMPlatform::GameBoyColor};
    auto dbl = vm.registerRoutine<std::uint16_t(std::uint16_t)>(
        routine("double_hl.asm"), RoutineBinding{.inputs = {gb::HL}, .output = gb::HL});
    EXPECT_EQ(dbl(0x1234), 0x2468);
}

TEST(AsmPipeline, MemoryBindingFromAsmFile) {
    Vm vm{VMPlatform::GameBoyColor};
    auto f = vm.registerRoutine<std::uint8_t(std::uint8_t)>(
        routine("mem_add.asm"),
        RoutineBinding{.inputs = {Location::memory(0xFF90)}, .output = Location::memory(0xFF91)});
    EXPECT_EQ(f(0x05), 0x15);
    EXPECT_EQ(f(0x20), 0x30);
}

TEST(AsmPipeline, HardwareSymbolResolvesFromAsmFile) {
    // rDIV resolves through the backend's GB hardware symbol table; two fresh, identically-stepped
    // VMs produce the same first roll (the rDIV read is deterministic on the emulated core).
    auto firstRoll = []() {
        Vm vm{VMPlatform::GameBoyColor};
        auto rng = vm.registerRoutine<std::uint8_t()>(routine("div_read.asm"),
                                                      RoutineBinding{.output = gb::A});
        return rng();
    };
    EXPECT_EQ(firstRoll(), firstRoll());
}

TEST(AsmPipeline, MissingFileThrows) {
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((vm.registerRoutine<std::uint8_t()>(routine("does_not_exist.asm"),
                                                     RoutineBinding{.output = gb::A})),
                 std::runtime_error);
}

TEST(AsmPipeline, BadSourceFileThrows) {
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((vm.registerRoutine<std::uint8_t()>(routine("bad_routine.asm"),
                                                     RoutineBinding{.output = gb::A})),
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
