#!/usr/bin/env python3
"""Author the asset-embed demo assets: a FONT sheet, a MENU-border sheet, and a 16-bit
GRAYSCALE map PNG — the same HELLO/WORLD-in-a-menu scene as the tilemap demo, but used to show
the per-asset embed policy (asset_embed_demo.cpp): the MAP and the FONT are baked into the binary
(Embed) while the MENU ships beside the binary and loads from disk (LoadFromPath).

These three files live in examples/embed_demo_assets/ — NOT examples/assets/ — on purpose: the shared
demo-asset copy step copies examples/assets/ next to every demo binary, which would put the embedded
files on disk too and defeat the demo's point (an embedded asset must NOT ship). Living here, the build
bakes the map+font from the source tree and only the menu is copied beside the binary.

  - asset_embed_demo_font.png  — indexed 8-bit: 8 cells (space, H, E, L, O, W, R, D), one 8x8 glyph each.
  - asset_embed_demo_menu.png  — indexed 8-bit: 4 cells (corner, h-edge, v-edge, fill) — the flip-
                                 irreducible minimum for a rectangular border.
  - asset_embed_demo_map.png   — 16-bit GRAYSCALE: each pixel's uint16 sample IS a catalog id (sparse,
                                 stride 4369 across 0..65535 — actually exercises 16-bit + stays
                                 eyeball-verifiable). Mirrored in asset_embed_demo.cpp.

Dependency-free PNG encoder (zlib + struct), same approach as examples/assets/gen_tilemap_demo.py.
Run from the engine repo root:  python3 examples/embed_demo_assets/gen_embed_demo.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent  # engine/examples/embed_demo_assets

STRIDE = 65535 // 15  # = 4369; catalog index k has id = STRIDE*k, spanning the full 16-bit range


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _png(width: int, height: int, bitdepth: int, colortype: int,
         scanlines: bytes, palette: list[tuple[int, int, int]] | None) -> bytes:
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, colortype, 0, 0, 0)
    out = sig + _chunk(b"IHDR", ihdr)
    if palette is not None:
        out += _chunk(b"PLTE", b"".join(struct.pack(">BBB", *c) for c in palette))
    out += _chunk(b"IDAT", zlib.compress(scanlines, 9))
    out += _chunk(b"IEND", b"")
    return out


GLYPH_PLTE = [(24, 30, 60), (255, 255, 255)]  # index 0 box/background, index 1 glyph ink


def write_indexed8(path: Path, glyphs: list[list[str]]) -> None:
    """A horizontal strip of 8x8 glyphs, '#' -> index 1, '.' -> index 0. 8-bit palette PNG."""
    h, w = 8, 8 * len(glyphs)
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter 0
        for g in glyphs:
            for x in range(8):
                raw.append(1 if g[y][x] == "#" else 0)
    path.write_bytes(_png(w, h, 8, 3, bytes(raw), GLYPH_PLTE))


def write_map16(path: Path, grid: list[list[int]]) -> None:
    """16-bit grayscale PNG: each sample is one big-endian uint16 — the catalog id at that cell."""
    h, w = len(grid), len(grid[0])
    raw = bytearray()
    for row in grid:
        raw.append(0)  # filter 0
        for v in row:
            raw.extend(struct.pack(">H", v & 0xFFFF))
    path.write_bytes(_png(w, h, 16, 0, bytes(raw), None))


# ── Font glyphs (8x8) — slot 0 space, then H E L O W R D (slots 1..7) ─────────────────────────────
SP = ["........"] * 8
H = ["........", ".#....#.", ".#....#.", ".######.", ".#....#.", ".#....#.", ".#....#.", "........"]
E = ["........", ".#####..", ".#......", ".####...", ".#......", ".#......", ".#####..", "........"]
L = ["........", ".#......", ".#......", ".#......", ".#......", ".#......", ".#####..", "........"]
O = ["........", "..###...", ".#...#..", ".#...#..", ".#...#..", ".#...#..", "..###...", "........"]
W = ["........", ".#...#..", ".#...#..", ".#.#.#..", ".#.#.#..", ".##.##..", ".#...#..", "........"]
R = ["........", ".####...", ".#...#..", ".####...", ".#.#....", ".#..#...", ".#...#..", "........"]
D = ["........", ".###....", ".#..#...", ".#...#..", ".#...#..", ".#..#...", ".###....", "........"]
FONT = [SP, H, E, L, O, W, R, D]

# ── Menu glyphs (8x8) — '#' = border line (index 1), '.' = box interior (index 0) ─────────────────
CORNER = ["########", "#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#......."]
HEDGE  = ["########", "........", "........", "........", "........", "........", "........", "........"]
VEDGE  = ["#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#.......", "#......."]
FILL   = ["........"] * 8
MENU = [CORNER, HEDGE, VEDGE, FILL]

# ── Catalog: index k → tile; the map stores id = STRIDE*k. (Mirrored in asset_embed_demo.cpp.) ─────
FILL_, TL, TR, BL, BR, TOP, BOT, LFT, RGT = range(9)
GLYPH = {"H": 9, "E": 10, "L": 11, "O": 12, "W": 13, "R": 14, "D": 15}

MAP_W, MAP_H = 20, 18  # a Game Boy 160x144 viewport in 8px tiles


def build_index_grid() -> list[list[int]]:
    g = [[FILL_ for _ in range(MAP_W)] for _ in range(MAP_H)]
    g[0][0], g[0][MAP_W - 1] = TL, TR
    g[MAP_H - 1][0], g[MAP_H - 1][MAP_W - 1] = BL, BR
    for x in range(1, MAP_W - 1):
        g[0][x], g[MAP_H - 1][x] = TOP, BOT
    for y in range(1, MAP_H - 1):
        g[y][0], g[y][MAP_W - 1] = LFT, RGT
    for i, ch in enumerate("HELLO"):
        g[3][3 + i] = GLYPH[ch]
    for i, ch in enumerate("WORLD"):
        g[5][3 + i] = GLYPH[ch]
    return g


def main() -> None:
    write_indexed8(HERE / "asset_embed_demo_font.png", FONT)
    write_indexed8(HERE / "asset_embed_demo_menu.png", MENU)
    grid = build_index_grid()
    id_grid = [[STRIDE * v for v in row] for row in grid]
    write_map16(HERE / "asset_embed_demo_map.png", id_grid)
    print(f"wrote asset_embed_demo_font.png (8 cells), asset_embed_demo_menu.png (4 cells), "
          f"asset_embed_demo_map.png ({MAP_W}x{MAP_H} 16-bit grayscale, ids = index*{STRIDE})")


if __name__ == "__main__":
    main()
