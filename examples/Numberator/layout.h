// Numberator — keypad layout + hit-testing. Pure geometry, header-only: the window size, the display
// well rectangle (mirroring the one baked into the chrome map), the 4x5 key grid, and the font's glyph
// order. Shared by the asset loader, the renderer, and the input handler so the numbers live in one place.
#pragma once

#include <array>

#include "retropp/geometry.h"  // Vec2i

namespace numberator {

// The window — a custom viewport, sized to the calculator (a multiple of 8 on both axes).
inline constexpr int kViewW = 248;
inline constexpr int kViewH = 344;

// The title bar height, in viewport pixels — the top strip of the chrome map (3 tile rows of 8px;
// mirrors gen_numberator_assets.py TITLE_ROWS). The drag handle covers this strip minus the close box.
inline constexpr int kTitleH = 24;

// The close box, in viewport pixels — the raised square the chrome map paints at cell (1,1)
// (mirrors gen_numberator_assets.py chrome_id's close-box cell; one 8px tile). Clicking it closes the
// window; the drag handle leaves its column alone so the click is a click, not a drag.
inline constexpr int kCloseX = 8, kCloseY = 8, kCloseW = 8, kCloseH = 8;

// True when a viewport-pixel point is on the close box.
inline bool onCloseBox(retropp::Vec2i p) {
    return p.x >= kCloseX && p.x < kCloseX + kCloseW && p.y >= kCloseY && p.y < kCloseY + kCloseH;
}

// The display well interior, in viewport pixels — where the digits draw. Mirrors the sunken well the
// chrome map paints (gen_numberator_assets.py: WELL cells 2..28 x 5..10).
inline constexpr int kDispX = 16, kDispY = 40, kDispW = 216, kDispH = 48;

// The key grid. Keys are SPRITES placed over the chrome, so their rectangle lives here (not in the map).
inline constexpr int kCols = 4, kRows = 5;
inline constexpr int kBtnW = 48, kBtnH = 40;
inline constexpr int kBtnX0 = 16, kBtnY0 = 104, kBtnGapX = 8, kBtnGapY = 8;

inline constexpr int keyX(int col) { return kBtnX0 + col * (kBtnW + kBtnGapX); }
inline constexpr int keyY(int row) { return kBtnY0 + row * (kBtnH + kBtnGapY); }

// What a key does when pressed.
enum class Action { Digit, Decimal, Op, Equals, Clear, Negate, Percent, Backspace };

struct Key {
    char   glyph;   // the font glyph drawn on the key (see glyphSlot)
    Action action;
    char   data;    // Digit -> '0'..'9'; Op -> '+' '-' '*' '/'; otherwise 0
    bool   isOp;    // true -> the darker function-key sprite, false -> the number-key sprite
};

// The 4x5 keypad, row-major (index = col + row*kCols), matching the on-screen grid:
//   C  ±  %  ÷        (~ is plus/minus, * is times, / is divide)
//   7  8  9  ×
//   4  5  6  −
//   1  2  3  +
//   0  .  ⌫  =        (< is backspace)
inline constexpr std::array<Key, 20> kKeys{{
    {'C', Action::Clear, 0, true},  {'~', Action::Negate, 0, true},   {'%', Action::Percent, 0, true},   {'/', Action::Op, '/', true},
    {'7', Action::Digit, '7', false}, {'8', Action::Digit, '8', false}, {'9', Action::Digit, '9', false}, {'*', Action::Op, '*', true},
    {'4', Action::Digit, '4', false}, {'5', Action::Digit, '5', false}, {'6', Action::Digit, '6', false}, {'-', Action::Op, '-', true},
    {'1', Action::Digit, '1', false}, {'2', Action::Digit, '2', false}, {'3', Action::Digit, '3', false}, {'+', Action::Op, '+', true},
    {'0', Action::Digit, '0', false}, {'.', Action::Decimal, 0, false}, {'<', Action::Backspace, 0, false}, {'=', Action::Equals, 0, true},
}};

// The key under a viewport-pixel point, or -1. (Sprites are placed at keyX/keyY, kBtnW x kBtnH.)
inline int keyAt(retropp::Vec2i p) {
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const int x = keyX(col), y = keyY(row);
            if (p.x >= x && p.x < x + kBtnW && p.y >= y && p.y < y + kBtnH) return row * kCols + col;
        }
    }
    return -1;
}

// The font slot for a glyph char. The font sheet's order is 0..9 then . - + * / = ~ % C < (20 slots).
inline int glyphSlot(char c) {
    switch (c) {
        case '.': return 10; case '-': return 11; case '+': return 12; case '*': return 13;
        case '/': return 14; case '=': return 15; case '~': return 16; case '%': return 17;
        case 'C': return 18; case '<': return 19;
        default:  return c - '0';  // '0'..'9'
    }
}

}  // namespace numberator
