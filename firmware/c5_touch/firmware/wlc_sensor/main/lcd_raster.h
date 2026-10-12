// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lcd_model.h"
#include "lcd_font.h"
static const uint16_t LCD_PALETTE[6] = {0x07ff,0xf81f,0xffe0,0x07e0,0x041f,0xf800};
static inline uint16_t lcd_pixel(const LcdFrame& frame, unsigned x, unsigned y) {
    unsigned row = y / 19, ly = y % 19;
    if (frame.identify && y >= 38 && y < 150) {
        unsigned count = frame.identify >= 10 ? 2 : 1, left = (320 - count * 48) / 2;
        if (x >= left && x < left + count * 48 && y >= 55 && y < 111) {
            unsigned dx = x - left;
            char digit = '0' + (count == 2 && dx / 48 == 0 ? frame.identify / 10 : frame.identify % 10);
            if (lcd_glyph(digit, (dx % 48) / 8) & (1 << ((y - 55) / 8))) return LCD_PALETTE[frame.palette];
        }
        return 0;
    }
    uint16_t bg = row == 1 ? frame.accent : row == 0 || row == 2 || row == 8 ? 0x1082 : 0;
    uint16_t fg = row == 1 ? 0 : 0xffff;
    if (row < 9 && x >= 4 && ly >= 2 && ly < 16) {
        unsigned dx = x - 4, index = dx / 12;
        if (index < frame.lengths[row] &&
            (lcd_glyph(frame.rows[row][index], (dx % 12) / 2) & (1 << ((ly - 2) / 2)))) return fg;
    }
    return bg;
}
