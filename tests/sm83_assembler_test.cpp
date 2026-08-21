// The in-engine SM83 assembler, exercised in isolation. Every case asserts the exact
// machine-code bytes the published SM83 opcode map specifies for the source — the golden-byte gate
// that lets the VM trust the encoder before any routine is registered through it.
#include "src/vm/gameboy/sm83_assembler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace retropp::vm {
namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes asm_(std::string_view src, const SymbolTable& syms = {}) {
    return assembleSm83(src, syms).bytes;
}

// ── The assembler is `constexpr` — the COMPILER assembles bytecode at build time ────────
// This is the property the Embed routine path depends on: a routine's machine code is computed during
// compilation and baked into the binary as a literal array — no runtime assembly, no .asm file shipped.
// If assembleSm83 were not a constant expression, this static_assert would fail to COMPILE (a
// build-time red→green: revert any non-constexpr op in the assembler and this stops compiling).
constexpr bool assemblesAtCompileTime() {
    const AssembledRoutine r = assembleSm83("start:\n  ld a,$10\n  inc a\n  jr nz, start\n  ret\n");
    const std::uint8_t     expected[] = {0x3E, 0x10, 0x3C, 0x20, 0xFB, 0xC9};
    if (r.bytes.size() != sizeof(expected)) return false;
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        if (r.bytes[i] != expected[i]) return false;
    }
    return r.labels.at("start") == 0u;  // labels resolve at compile time too
}
static_assert(assemblesAtCompileTime(), "SM83 assembler must assemble bytecode at compile time");

// The embed invariant: bytecode BAKED at compile time (the two-phase constexpr-vector→constexpr-array
// pattern the Embed path uses) is byte-identical to assembling the same source at RUNTIME (what a
// LoadFromPath routine does). One assembler, two evaluation times, identical bytes.
TEST(Sm83Assembler, CompileTimeBytecodeEqualsRuntime) {
    constexpr std::string_view   src = "loop:\n  ld a,[$ff04]\n  add a,b\n  jr loop\n";
    static constexpr std::size_t kN  = assembleSm83(src).bytes.size();  // phase 1: size (vector freed)
    static constexpr std::array<std::uint8_t, kN> kBaked = [src] {       // phase 2: fill the array
        std::array<std::uint8_t, kN> a{};
        const AssembledRoutine       r = assembleSm83(src);
        for (std::size_t i = 0; i < kN; ++i) a[i] = r.bytes[i];
        return a;
    }();
    const Bytes runtime = assembleSm83(src).bytes;
    ASSERT_EQ(runtime.size(), kBaked.size());
    EXPECT_TRUE(std::equal(runtime.begin(), runtime.end(), kBaked.begin()));
}

// ── 8-bit register loads (the 0x40..0x7F block, [hl] = r-code 6) ──────────────────────────────
TEST(Sm83Assembler, EightBitRegisterLoads) {
    EXPECT_EQ(asm_("ld a,b"), (Bytes{0x78}));
    EXPECT_EQ(asm_("ld b,a"), (Bytes{0x47}));
    EXPECT_EQ(asm_("ld a,[hl]"), (Bytes{0x7E}));
    EXPECT_EQ(asm_("ld [hl],a"), (Bytes{0x77}));
    EXPECT_EQ(asm_("ld b,[hl]"), (Bytes{0x46}));
    EXPECT_EQ(asm_("ld c,d"), (Bytes{0x4A}));
}

TEST(Sm83Assembler, ImmediateLoads) {
    EXPECT_EQ(asm_("ld a,$42"), (Bytes{0x3E, 0x42}));
    EXPECT_EQ(asm_("ld b,5"), (Bytes{0x06, 0x05}));
    EXPECT_EQ(asm_("ld [hl],$ff"), (Bytes{0x36, 0xFF}));
    EXPECT_EQ(asm_("ld l,%00001111"), (Bytes{0x2E, 0x0F}));
}

TEST(Sm83Assembler, IndirectAndAbsoluteLoads) {
    EXPECT_EQ(asm_("ld a,[bc]"), (Bytes{0x0A}));
    EXPECT_EQ(asm_("ld a,[de]"), (Bytes{0x1A}));
    EXPECT_EQ(asm_("ld [bc],a"), (Bytes{0x02}));
    EXPECT_EQ(asm_("ld [de],a"), (Bytes{0x12}));
    EXPECT_EQ(asm_("ld a,[hl+]"), (Bytes{0x2A}));
    EXPECT_EQ(asm_("ld a,[hl-]"), (Bytes{0x3A}));
    EXPECT_EQ(asm_("ld [hl+],a"), (Bytes{0x22}));
    EXPECT_EQ(asm_("ld [hld],a"), (Bytes{0x32}));
    EXPECT_EQ(asm_("ld a,[$1234]"), (Bytes{0xFA, 0x34, 0x12}));   // little-endian
    EXPECT_EQ(asm_("ld [$1234],a"), (Bytes{0xEA, 0x34, 0x12}));
}

TEST(Sm83Assembler, HighPageLoads) {
    EXPECT_EQ(asm_("ldh a,[$ff04]"), (Bytes{0xF0, 0x04}));   // full $FFxx → low byte
    EXPECT_EQ(asm_("ldh [$ff04],a"), (Bytes{0xE0, 0x04}));
    EXPECT_EQ(asm_("ldh a,[$80]"), (Bytes{0xF0, 0x80}));     // bare low byte
    EXPECT_EQ(asm_("ldh a,[c]"), (Bytes{0xF2}));
    EXPECT_EQ(asm_("ldh [c],a"), (Bytes{0xE2}));
}

TEST(Sm83Assembler, SixteenBitLoads) {
    EXPECT_EQ(asm_("ld bc,$1234"), (Bytes{0x01, 0x34, 0x12}));
    EXPECT_EQ(asm_("ld de,$1234"), (Bytes{0x11, 0x34, 0x12}));
    EXPECT_EQ(asm_("ld hl,$1234"), (Bytes{0x21, 0x34, 0x12}));
    EXPECT_EQ(asm_("ld sp,$fffe"), (Bytes{0x31, 0xFE, 0xFF}));
    EXPECT_EQ(asm_("ld sp,hl"), (Bytes{0xF9}));
    EXPECT_EQ(asm_("ld hl,sp+5"), (Bytes{0xF8, 0x05}));
    EXPECT_EQ(asm_("ld [$1234],sp"), (Bytes{0x08, 0x34, 0x12}));
}

// ── 8-bit ALU (A-implicit; `op r`, `op a,r`, `op [hl]`, `op n` all encode the same) ────────────
TEST(Sm83Assembler, EightBitAlu) {
    EXPECT_EQ(asm_("add b"), (Bytes{0x80}));
    EXPECT_EQ(asm_("add a,b"), (Bytes{0x80}));       // explicit A form == implicit
    EXPECT_EQ(asm_("add [hl]"), (Bytes{0x86}));
    EXPECT_EQ(asm_("add 5"), (Bytes{0xC6, 0x05}));
    EXPECT_EQ(asm_("adc b"), (Bytes{0x88}));
    EXPECT_EQ(asm_("sub c"), (Bytes{0x91}));
    EXPECT_EQ(asm_("sbc b"), (Bytes{0x98}));
    EXPECT_EQ(asm_("and a"), (Bytes{0xA7}));
    EXPECT_EQ(asm_("xor a"), (Bytes{0xAF}));
    EXPECT_EQ(asm_("or b"), (Bytes{0xB0}));
    EXPECT_EQ(asm_("cp $10"), (Bytes{0xFE, 0x10}));
}

TEST(Sm83Assembler, IncDec) {
    EXPECT_EQ(asm_("inc a"), (Bytes{0x3C}));
    EXPECT_EQ(asm_("inc [hl]"), (Bytes{0x34}));
    EXPECT_EQ(asm_("dec b"), (Bytes{0x05}));
    EXPECT_EQ(asm_("inc hl"), (Bytes{0x23}));
    EXPECT_EQ(asm_("dec bc"), (Bytes{0x0B}));
    EXPECT_EQ(asm_("inc sp"), (Bytes{0x33}));
}

TEST(Sm83Assembler, SixteenBitArithmetic) {
    EXPECT_EQ(asm_("add hl,bc"), (Bytes{0x09}));
    EXPECT_EQ(asm_("add hl,de"), (Bytes{0x19}));
    EXPECT_EQ(asm_("add hl,hl"), (Bytes{0x29}));
    EXPECT_EQ(asm_("add hl,sp"), (Bytes{0x39}));
    EXPECT_EQ(asm_("add sp,-2"), (Bytes{0xE8, 0xFE}));   // signed displacement
}

TEST(Sm83Assembler, PushPop) {
    EXPECT_EQ(asm_("push bc"), (Bytes{0xC5}));
    EXPECT_EQ(asm_("push af"), (Bytes{0xF5}));
    EXPECT_EQ(asm_("pop hl"), (Bytes{0xE1}));
    EXPECT_EQ(asm_("pop af"), (Bytes{0xF1}));
}

// ── single-byte misc / rotates ────────────────────────────────────────────────────────────────
TEST(Sm83Assembler, SingleByteOps) {
    EXPECT_EQ(asm_("nop"), (Bytes{0x00}));
    EXPECT_EQ(asm_("halt"), (Bytes{0x76}));
    EXPECT_EQ(asm_("stop"), (Bytes{0x10, 0x00}));
    EXPECT_EQ(asm_("di"), (Bytes{0xF3}));
    EXPECT_EQ(asm_("ei"), (Bytes{0xFB}));
    EXPECT_EQ(asm_("ccf"), (Bytes{0x3F}));
    EXPECT_EQ(asm_("scf"), (Bytes{0x37}));
    EXPECT_EQ(asm_("cpl"), (Bytes{0x2F}));
    EXPECT_EQ(asm_("daa"), (Bytes{0x27}));
    EXPECT_EQ(asm_("rlca"), (Bytes{0x07}));
    EXPECT_EQ(asm_("rra"), (Bytes{0x1F}));
    EXPECT_EQ(asm_("reti"), (Bytes{0xD9}));
}

// ── jumps / calls / returns / rst ─────────────────────────────────────────────────────────────
TEST(Sm83Assembler, JumpsCallsReturns) {
    EXPECT_EQ(asm_("jp $1234"), (Bytes{0xC3, 0x34, 0x12}));
    EXPECT_EQ(asm_("jp nz,$1234"), (Bytes{0xC2, 0x34, 0x12}));
    EXPECT_EQ(asm_("jp c,$1234"), (Bytes{0xDA, 0x34, 0x12}));
    EXPECT_EQ(asm_("jp hl"), (Bytes{0xE9}));
    EXPECT_EQ(asm_("call $1234"), (Bytes{0xCD, 0x34, 0x12}));
    EXPECT_EQ(asm_("call z,$1234"), (Bytes{0xCC, 0x34, 0x12}));
    EXPECT_EQ(asm_("ret"), (Bytes{0xC9}));
    EXPECT_EQ(asm_("ret z"), (Bytes{0xC8}));
    EXPECT_EQ(asm_("ret nc"), (Bytes{0xD0}));
    EXPECT_EQ(asm_("rst $00"), (Bytes{0xC7}));
    EXPECT_EQ(asm_("rst $38"), (Bytes{0xFF}));
}

// ── CB-prefixed ───────────────────────────────────────────────────────────────────────────────
TEST(Sm83Assembler, CbPrefixedOps) {
    EXPECT_EQ(asm_("rlc b"), (Bytes{0xCB, 0x00}));
    EXPECT_EQ(asm_("rr a"), (Bytes{0xCB, 0x1F}));
    EXPECT_EQ(asm_("sla [hl]"), (Bytes{0xCB, 0x26}));
    EXPECT_EQ(asm_("swap a"), (Bytes{0xCB, 0x37}));
    EXPECT_EQ(asm_("srl a"), (Bytes{0xCB, 0x3F}));
    EXPECT_EQ(asm_("bit 7,a"), (Bytes{0xCB, 0x7F}));
    EXPECT_EQ(asm_("res 0,b"), (Bytes{0xCB, 0x80}));
    EXPECT_EQ(asm_("set 3,[hl]"), (Bytes{0xCB, 0xDE}));
}

// ── labels, jr resolution, comments, predefined symbols ───────────────────────────────────────
TEST(Sm83Assembler, BackwardJrResolves) {
    // loop: nop ; jr loop   — jr at offset 1, after = 3, target 0 → rel -3 = 0xFD
    EXPECT_EQ(asm_("loop:\n  nop\n  jr loop\n"), (Bytes{0x00, 0x18, 0xFD}));
}

TEST(Sm83Assembler, ForwardConditionalJrResolves) {
    // jr nz,skip (off 0, after 2) ; nop (off 2) ; skip: (off 3) → rel +1
    EXPECT_EQ(asm_("  jr nz,skip\n  nop\nskip:\n"), (Bytes{0x20, 0x01, 0x00}));
}

TEST(Sm83Assembler, CommentsAndBlankLinesIgnored) {
    EXPECT_EQ(asm_("  ; just a comment\n\n  nop   ; trailing\n"), (Bytes{0x00}));
}

TEST(Sm83Assembler, PredefinedSymbolsResolve) {
    SymbolTable syms{{"rdiv", 0xFF04}, {"seedadd", 0xFFE1}};
    EXPECT_EQ(asm_("ldh a,[rDIV]", syms), (Bytes{0xF0, 0x04}));
    EXPECT_EQ(asm_("ldh [seedAdd],a", syms), (Bytes{0xE0, 0xE1}));
}

TEST(Sm83Assembler, LabelOffsetsExported) {
    const AssembledRoutine r = assembleSm83("entry:\n  nop\n  nop\ntail:\n  ret\n");
    EXPECT_EQ(r.bytes, (Bytes{0x00, 0x00, 0xC9}));
    ASSERT_TRUE(r.labels.count("entry"));
    ASSERT_TRUE(r.labels.count("tail"));
    EXPECT_EQ(r.labels.at("entry"), 0u);
    EXPECT_EQ(r.labels.at("tail"), 2u);
}

// ── the engine's own RNG presets (the dogfood golden) ─────────────────────────────────────────
// These ARE the byte arrays gb_routines.h ships; assembling the readable source must reproduce them
// exactly. Break a byte in the source → these go red.
TEST(Sm83Assembler, DivRngPresetBytes) {
    SymbolTable syms{{"rdiv", 0xFF04}};
    EXPECT_EQ(asm_("ldh a,[rDIV]\nret\n", syms), (Bytes{0xF0, 0x04, 0xC9}));
}

TEST(Sm83Assembler, DualSeedRngPresetBytes) {
    SymbolTable syms{{"rdiv", 0xFF04}, {"seedadd", 0xFFE1}, {"seedsub", 0xFFE2}};
    const char* src =
        "ldh a,[seedAdd]\n"
        "ld b,a\n"
        "ldh a,[rDIV]\n"
        "adc b\n"
        "ldh [seedAdd],a\n"
        "ldh a,[seedSub]\n"
        "ld b,a\n"
        "ldh a,[rDIV]\n"
        "sbc b\n"
        "ldh [seedSub],a\n"
        "ret\n";
    EXPECT_EQ(asm_(src, syms), (Bytes{0xF0, 0xE1, 0x47, 0xF0, 0x04, 0x88, 0xE0, 0xE1,
                                      0xF0, 0xE2, 0x47, 0xF0, 0x04, 0x98, 0xE0, 0xE2, 0xC9}));
}

// ── error paths ───────────────────────────────────────────────────────────────────────────────
TEST(Sm83Assembler, UnknownMnemonicThrows) {
    EXPECT_THROW(assembleSm83("frobnicate a,b\n"), std::runtime_error);
}

TEST(Sm83Assembler, UnknownSymbolThrows) {
    EXPECT_THROW(assembleSm83("ld a,[neverDefined]\n"), std::runtime_error);
}

TEST(Sm83Assembler, JrOutOfRangeThrows) {
    // 200 nops then a backward jr to the top — well beyond a signed byte.
    std::string src = "top:\n";
    for (int i = 0; i < 200; ++i) src += "  nop\n";
    src += "  jr top\n";
    EXPECT_THROW(assembleSm83(src), std::runtime_error);
}

TEST(Sm83Assembler, BadOperandShapeThrows) {
    EXPECT_THROW(assembleSm83("ld a\n"), std::runtime_error);          // missing operand
    EXPECT_THROW(assembleSm83("push a\n"), std::runtime_error);        // A is not a pair
    EXPECT_THROW(assembleSm83("bit 9,a\n"), std::runtime_error);       // bit index > 7
}

}  // namespace
}  // namespace retropp::vm
