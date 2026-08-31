#pragma once

// A named place in a target machine's address space: where it starts, how big one entry is, and how
// many entries there are.
//
// This is the vocabulary for reaching into the guest's memory — to read a hosted ROM's own assets, or to
// look at what a running machine is doing. It describes WHERE, and nothing else. The bytes it addresses
// are bytes; what they mean is the caller's.
//
// ONE TYPE, TWO KINDS OF AUTHOR. A console's header ships constants for the machine's own hardware
// memories (gb::Rom, gb::VRam, …) exactly as it ships gb::A for Location; a game declares its own for
// the places its content actually lives. They are not different concepts — they are the same value
// filled in by different people, so anything that reads one reads the other.
//
// PLATFORM-NEUTRAL. `at` is an opaque address in the target machine's own space, 32-bit so a console
// with a wider bus (or a bank-qualified address) fits without a surface change. A console's header
// supplies whatever folding its hardware needs — on the Game Boy family that is gb::banked(bank,
// addr16), the same encoding a placed driver image's base already uses — and the backend decodes it.
// The platform never learns what the bits mean.
//
// A REGION IS A PLAIN VALUE, constructible at runtime. Much real content is not tabular: a pointer table
// points at variable-length blobs, and reaching one means reading the table, decoding an entry, and
// building a region from what you just read. That has to be expressible, so nothing here is
// declaration-only.
//
// IT DESCRIBES WHERE, NEVER HOW BYTES DIVIDE INTO PICTURES. `size` and `count` exist so an indexed read
// can address entry N without reading the other N-1 — they are lazy addressing, not a layout. How a flat
// extent divides into cells is the atlas carve's job (retropp/image.h, sliceSlot), which already
// partitions bytes once they are in hand and does not care where they came from. Read a region, then
// carve what you got.

#include <cstddef>
#include <cstdint>

namespace retropp {

// Where something lives in the guest's address space.
//
//   at    — the entry's starting address in the machine's own space. Bank-qualified where the console
//           needs it (gb::banked). Opaque to the platform; the backend decodes it.
//   size  — bytes in ONE entry. For a whole hardware memory this is that memory's length.
//   count — how many consecutive entries of `size` there are. Defaults to 1: a single blob, and a whole
//           hardware memory, are the degenerate case of an array with one element.
//
// Entry N is not simply `at + N * size` on hardware that banks — a long array runs off the end of the
// switchable window rather than continuing — so the stride is resolved by the backend in decoded
// address space, never by arithmetic on `at`.
struct MemoryRegion {
    std::uint32_t at    = 0;
    std::uint32_t size  = 0;
    std::uint32_t count = 1;

    // Total bytes the region spans across all its entries.
    [[nodiscard]] constexpr std::uint64_t totalBytes() const noexcept {
        return static_cast<std::uint64_t>(size) * count;
    }

    // Whether `index` names an entry this region declares. An indexed read past the end throws rather
    // than returning whatever follows in memory.
    [[nodiscard]] constexpr bool contains(std::uint32_t index) const noexcept { return index < count; }
};

}  // namespace retropp
