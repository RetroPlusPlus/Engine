#!/usr/bin/env python3
"""Author the engine's tiny, license-clean PNG fixtures + the demo asset.

ENG-2.B.3.a needs committed PNGs with a KNOWN index plane so the headless image
decode test can assert exact values, and a small indexed tileset with index-0 holes
for the window demo's transparent-index check. These are engine-original images (not
Crystal/Nintendo art). This is a one-time authoring tool kept committed so the binary
fixtures stay regenerable and their exact index planes are auditable.

Dependency-free: a minimal PNG encoder over the standard library (zlib + struct), so it
runs anywhere Python does — no Pillow. Emits non-interlaced, filter-0 PNGs:
  - 8-bit PLTE (palette) images  → colortype 3
  - 2-bit grayscale images       → colortype 0  (the stored sample value IS the index)

Run from the engine repo root:  python3 tests/fixtures/gen_fixtures.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent          # engine/tests/fixtures
ENGINE = HERE.parent.parent                      # engine/
ASSETS = ENGINE / "examples" / "assets"


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width: int, height: int, bitdepth: int, colortype: int,
         scanlines: bytes, palette: list[tuple[int, int, int, int]] | None) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr)
    if palette is not None:
        plte = b"".join(struct.pack(">BBB", r, g, b) for (r, g, b, _a) in palette)
        out += _chunk(b"PLTE", plte)
        # tRNS carries per-index alpha; we keep palettes opaque, so it is omitted.
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9))
    out += _chunk(b"IEND", b"")
    return out


def write_indexed8(path: Path, width: int, height: int,
                   indices: list[list[int]], palette: list[tuple[int, int, int, int]]) -> None:
    """8-bit palette PNG: one index byte per pixel."""
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        raw.extend(row)
    path.write_bytes(_png(width, height, 8, 3, bytes(raw), palette))


def write_gray16(path: Path, width: int, height: int, values: list[list[int]]) -> None:
    """16-bit grayscale PNG: each sample is one big-endian uint16 (colortype 0, bitdepth 16). The
    stored sample value IS the index — used by the ENG-2.L map-import path, which must carry indices
    above 255 (a tilemap may reference >256 catalog entries)."""
    raw = bytearray()
    for row in values:
        raw.append(0)  # filter type 0 (none)
        for v in row:
            raw.extend(struct.pack(">H", v & 0xFFFF))  # PNG samples are big-endian
    path.write_bytes(_png(width, height, 16, 0, bytes(raw), None))


def write_gray2(path: Path, width: int, height: int, indices: list[list[int]]) -> None:
    """2-bit grayscale PNG: sample values 0..3 packed MSB-first, 4 px/byte."""
    raw = bytearray()
    for row in indices:
        raw.append(0)  # filter type 0 (none)
        acc = 0
        nbits = 0
        for v in row:
            acc = (acc << 2) | (v & 3)
            nbits += 2
            if nbits == 8:
                raw.append(acc)
                acc = 0
                nbits = 0
        if nbits:                       # flush a partial trailing byte (left-justified)
            raw.append(acc << (8 - nbits))
    path.write_bytes(_png(width, height, 2, 0, bytes(raw), None))


# A clear 4×4 diagonal pattern — every index appears, trivially hand-verifiable.
DIAGONAL_4x4 = [
    [0, 1, 2, 3],
    [1, 2, 3, 0],
    [2, 3, 0, 1],
    [3, 0, 1, 2],
]

# Four distinct opaque palette entries (consumer may ignore these — the demo hand-builds colour).
PALETTE4 = [(10, 20, 30, 255), (40, 50, 60, 255), (70, 80, 90, 255), (100, 110, 120, 255)]

# A 4×4 16-bit map plane (ENG-2.L): raw index values, several ABOVE 255 to prove the wide decode
# (an 8-bit path would truncate these). Spans 0 .. 65535 (the uint16 range), trivially hand-verifiable.
MAP16_4x4 = [
    [0,     1,     255,   256],
    [257,   300,   1000,  4095],
    [4096,  20000, 40000, 60000],
    [65535, 2,     513,   128],
]


def demo_tiles_indices() -> list[list[int]]:
    """16×16 (2×2-tile) indexed tileset with a central diamond of index 0 (holes) ringed by a
    coloured band — so the transparent-index demo shows the lower layer through an obvious shape."""
    rows = []
    for y in range(16):
        row = []
        for x in range(16):
            d = abs(x - 8) + abs(y - 8)         # Manhattan distance from centre
            if d <= 5:
                row.append(0)                   # central diamond → transparent hole
            else:
                row.append(1 + ((x // 2 + y // 2) % 3))  # 1..3 coloured bands
        rows.append(row)
    return rows


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)

    write_indexed8(HERE / "indexed4.png", 4, 4, DIAGONAL_4x4, PALETTE4)
    write_gray2(HERE / "gray2.png", 4, 4, DIAGONAL_4x4)
    write_gray16(HERE / "map16.png", 4, 4, MAP16_4x4)
    write_indexed8(ASSETS / "demo_tiles.png", 16, 16, demo_tiles_indices(), PALETTE4)

    # Print the exact index planes so image_test.cpp's expected values are transcribed, not guessed.
    print("indexed4.png / gray2.png index plane (row-major):")
    for r in DIAGONAL_4x4:
        print("  ", r)
    print("indexed4.png palette (RGBA):")
    for i, c in enumerate(PALETTE4):
        print(f"   {i}: {c}")
    print("demo_tiles.png 16x16 index plane:")
    for r in demo_tiles_indices():
        print("  ", "".join(str(v) for v in r))
    print("map16.png 4x4 uint16 value plane (row-major):")
    for r in MAP16_4x4:
        print("  ", r)


if __name__ == "__main__":
    main()
