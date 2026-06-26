#!/usr/bin/env python3
"""Author the engine's tiny, license-clean PNG fixtures + the demo asset.

The headless image-decode tests need committed PNGs with a KNOWN pixel/index plane so they can
assert exact values, plus a small indexed tileset with index-0 holes for the window demo's
transparent-index check. These are engine-original images (not Crystal/Nintendo art). This is a
one-time authoring tool kept committed so the binary fixtures stay regenerable and their exact
planes are auditable.

Dependency-free: a minimal PNG encoder over the standard library (zlib + struct), so it
runs anywhere Python does — no Pillow. Emits non-interlaced, filter-0 PNGs:
  - 8-bit PLTE (palette) images        → colortype 3
  - 2-/16-bit grayscale images         → colortype 0  (the stored sample value IS the index)
  - 8-/16-bit truecolour(-alpha) images → colortype 2 / 6  (one colour per pixel)

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
    stored sample value IS the index — used by the map-import path, which must carry indices above
    255 (a tilemap may reference >256 catalog entries)."""
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


def write_rgb8(path: Path, width: int, height: int,
               pixels: list[list[tuple[int, int, int]]]) -> None:
    """8-bit truecolour PNG (colortype 2): three bytes (R, G, B) per pixel, no alpha channel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b) in row:
            raw.extend((r & 0xFF, g & 0xFF, b & 0xFF))
    path.write_bytes(_png(width, height, 8, 2, bytes(raw), None))


def write_rgba8(path: Path, width: int, height: int,
                pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """8-bit truecolour-alpha PNG (colortype 6): four bytes (R, G, B, A) per pixel."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend((r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF))
    path.write_bytes(_png(width, height, 8, 6, bytes(raw), None))


def write_rgba16(path: Path, width: int, height: int,
                 pixels: list[list[tuple[int, int, int, int]]]) -> None:
    """16-bit truecolour-alpha PNG (colortype 6, bitdepth 16): four big-endian uint16 samples per
    pixel. Carries channel values above 0xFF00 so a decode that crushed to 8 bits would be caught."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (none)
        for (r, g, b, a) in row:
            raw.extend(struct.pack(">HHHH", r & 0xFFFF, g & 0xFFFF, b & 0xFFFF, a & 0xFFFF))
    path.write_bytes(_png(width, height, 16, 6, bytes(raw), None))


# A clear 4×4 diagonal pattern — every index appears, trivially hand-verifiable.
DIAGONAL_4x4 = [
    [0, 1, 2, 3],
    [1, 2, 3, 0],
    [2, 3, 0, 1],
    [3, 0, 1, 2],
]

# Four distinct opaque palette entries (consumer may ignore these — the demo hand-builds colour).
PALETTE4 = [(10, 20, 30, 255), (40, 50, 60, 255), (70, 80, 90, 255), (100, 110, 120, 255)]

# A 4×4 16-bit map plane: raw index values, several ABOVE 255 to prove the wide decode
# (an 8-bit path would truncate these). Spans 0 .. 65535 (the uint16 range), trivially hand-verifiable.
MAP16_4x4 = [
    [0,     1,     255,   256],
    [257,   300,   1000,  4095],
    [4096,  20000, 40000, 60000],
    [65535, 2,     513,   128],
]


# A 2×2 8-bit truecolour-alpha plane: includes a fully transparent pixel (a=0) so the decode is
# proven to carry alpha, and an opaque white so the channel endpoints (0 and 255) both appear.
RGBA8_2x2 = [
    [(0,  0,  0,  255), (255, 255, 255, 255)],
    [(10, 20, 30, 40),  (200, 100, 50,  0)],
]

# A 2×2 8-bit truecolour (no alpha) plane: decode must synthesize opaque alpha (→ 65535 widened).
RGB8_2x2 = [
    [(0,  0,  0),  (255, 128, 0)],
    [(12, 34, 56), (78,  90,  255)],
]

# A 2×2 16-bit truecolour-alpha plane: exact uint16 samples, several ABOVE 0xFF00 (e.g. 0xFF01,
# 0xFFFF) so an 8-bit crush would be caught, plus a transparent pixel (a=0). Hand-verifiable.
RGBA16_2x2 = [
    [(0,      65535,  257,    32896), (0xFFFF, 0xFF01, 0x0100, 0x00FF)],
    [(1000,   2000,   3000,   4000),  (65280,  60000,  65535,  0)],
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
    write_rgba8(HERE / "rgba8.png", 2, 2, RGBA8_2x2)
    write_rgb8(HERE / "rgb8.png", 2, 2, RGB8_2x2)
    write_rgba16(HERE / "rgba16.png", 2, 2, RGBA16_2x2)
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
    print("rgba8.png 2x2 (R,G,B,A) plane:")
    for r in RGBA8_2x2:
        print("  ", r)
    print("rgb8.png 2x2 (R,G,B) plane:")
    for r in RGB8_2x2:
        print("  ", r)
    print("rgba16.png 2x2 (R,G,B,A) uint16 plane:")
    for r in RGBA16_2x2:
        print("  ", r)


if __name__ == "__main__":
    main()
