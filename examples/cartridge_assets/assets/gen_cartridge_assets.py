#!/usr/bin/env python3
"""Author the demo cartridge for examples/cartridge_assets.

The cartridge is written here, byte for byte, so the example depends on no one else's ROM. SameBoy
reads only the cartridge-type and ROM-size header fields to size an image and pick a mapper, so a
usable cartridge is a planted pattern plus those two bytes.

Everything below is the plain library: no packages, no image tooling. Run it from the repo root:

    python3 examples/cartridge_assets/assets/gen_cartridge_assets.py

It rewrites demo_cartridge.gb in place. Both this script and its output are committed, so the
example builds without anyone running it.
"""

import os

CARTRIDGE_BYTES = 0x8000  # 32 KiB: the smallest a Game Boy cartridge can be, and plenty here

TILE_BASE = 0x1000  # where the tile art lives in the image
TEXT_BASE = 0x2000  # where the text table lives
TEXT_ENTRY = 8      # bytes per text entry

# Four 8x8 tiles in the shades a Game Boy tile can hold: 0 is the lightest, 3 the darkest. These are
# authored as pictures so the source reads as what it draws.
TILES = [
    # a face
    [
        "..3333..",
        ".322223.",
        "32133123",
        "32222223",
        "32133123",
        "32311323",
        ".322223.",
        "..3333..",
    ],
    # an arrow, pointing up
    [
        "...33...",
        "..3223..",
        ".322223.",
        "33322333",
        "...22...",
        "...22...",
        "...22...",
        "...33...",
    ],
    # a frame
    [
        "33333333",
        "31111113",
        "31222213",
        "31222213",
        "31222213",
        "31222213",
        "31111113",
        "33333333",
    ],
    # a heart
    [
        ".33..33.",
        "32233223",
        "32222223",
        "32222223",
        ".322223.",
        "..3223..",
        "...33...",
        "........",
    ],
]

# The text table the example reads through its second declared place. Eight bytes each, space-padded,
# in plain ASCII — the encoding is the example's own, exactly as a real game's would be.
TEXT_ENTRIES = ["FACE", "ARROW", "FRAME", "HEART"]


def tile_to_2bpp(rows):
    """One 8x8 tile as the 16 bytes the hardware stores: two bitplanes per row, low plane first."""
    out = bytearray()
    for row in rows:
        low = 0
        high = 0
        for x, cell in enumerate(row):
            value = 0 if cell == "." else int(cell)
            bit = 7 - x
            low |= (value & 1) << bit
            high |= ((value >> 1) & 1) << bit
        out.append(low)
        out.append(high)
    return bytes(out)


def build_cartridge():
    rom = bytearray(CARTRIDGE_BYTES)

    # The only header fields anything reads: cartridge type and ROM size. Size is log2(bytes / 32 KiB).
    rom[0x0147] = 0x00  # ROM ONLY, no mapper
    rom[0x0148] = 0x00  # 32 KiB
    rom[0x0149] = 0x00  # no cartridge RAM

    for index, tile in enumerate(TILES):
        assert len(tile) == 8 and all(len(r) == 8 for r in tile), "a tile is 8x8"
        rom[TILE_BASE + index * 16 : TILE_BASE + (index + 1) * 16] = tile_to_2bpp(tile)

    for index, text in enumerate(TEXT_ENTRIES):
        encoded = text.encode("ascii").ljust(TEXT_ENTRY, b" ")[:TEXT_ENTRY]
        rom[TEXT_BASE + index * TEXT_ENTRY : TEXT_BASE + (index + 1) * TEXT_ENTRY] = encoded

    return bytes(rom)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "demo_cartridge.gb")
    with open(path, "wb") as f:
        f.write(build_cartridge())
    print(f"wrote {path} ({CARTRIDGE_BYTES} bytes, {len(TILES)} tiles, {len(TEXT_ENTRIES)} names)")


if __name__ == "__main__":
    main()
