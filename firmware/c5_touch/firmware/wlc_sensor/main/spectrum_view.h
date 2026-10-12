// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen spectrum for the touch board's sensor image: a live trace with max-hold over a
// waterfall, 320x240. Pure pixel function over plain arrays so it can be checked on a host.
#pragma once
#include <stdint.h>
#include <string.h>
#include "lcd_font.h"
#include "spectrum_skirt.h"
#include <math.h>

struct SpectrumView {
    static constexpr unsigned W = 320, H = 240;
    static constexpr unsigned TITLE_H = 19, TRACE_Y = 20, TRACE_H = 128, AXIS_Y = 150, FALL_Y = 168;
    static constexpr unsigned FALL_ROWS = 72, FALL_COLS = 160;
    uint8_t trace[W];            // dB per column, latest frame
    uint8_t hold[W];             // decaying max-hold
    uint8_t floor_db = 40, span_db = 60;
    uint8_t* fall = nullptr;     // FALL_ROWS x FALL_COLS, ring; optional
    unsigned fall_head = 0;      // row index of the newest line
    char title[27] = {};
    char axis[27] = {};

    // A view is up to three frequency ranges laid side by side (non-adjacent bands get a divider).
    // Captures are tuned every 20 MHz across each range at an 80 MHz sample rate; only the flat
    // middle +-20 MHz of a capture is used. Each column takes its value from the capture in which
    // that frequency lies farthest from the capture's own centre, so the receiver's DC/LO artifacts
    // (3 bins at 2.4 GHz, up to +-2.5 MHz of spurs at 5 GHz) never reach the screen.
    struct Range { uint16_t lo_mhz, hi_mhz, x0, x1; };
    Range ranges[3] = {};
    uint8_t range_count = 0;
    uint8_t sep[W] = {};          // 1 = draw a divider
    uint8_t cand[W] = {};         // this sweep's best value per column
    int16_t cand_khz[W] = {};     // its distance from DC; -1 = none yet

    void set_ranges(const uint16_t (*r)[2], unsigned n) {
        range_count = n > 3 ? 3 : n;
        unsigned total = 0;
        for (unsigned i = 0; i < range_count; i++) total += r[i][1] - r[i][0];
        unsigned x = 0;
        for (unsigned i = 0; i < range_count; i++) {
            unsigned w = i + 1 == range_count ? W - x : (unsigned)(r[i][1] - r[i][0]) * W / total;
            ranges[i] = {r[i][0], r[i][1], (uint16_t)x, (uint16_t)(x + w)};
            if (i) sep[x] = 1;
            x += w;
        }
        for (unsigned c = 0; c < W; c++) cand_khz[c] = -1;
    }
    // Removes the receiver's own noise skirt around DC (spectrum_skirt.h) in linear power, so the
    // floor is flat across a capture while real signals keep their level.
    static uint8_t deskirt(uint8_t v, unsigned dist_bins, bool is_5ghz) {
        const uint8_t* table = is_5ghz ? SKIRT_5 : SKIRT_24;
        const uint8_t ref = is_5ghz ? SKIRT_5_REF : SKIRT_24_REF;
        uint8_t skirt = table[dist_bins > 64 ? 64 : dist_bins];
        if (skirt <= ref) return v;
        float p = powf(10.f, v / 20.f) - (powf(10.f, skirt / 20.f) - powf(10.f, ref / 20.f));
        float floor_p = powf(10.f, ref / 20.f) * 0.5f;
        float out = 20.f * log10f(p > floor_p ? p : floor_p);
        return out <= 0 ? 0 : out >= 255 ? 255 : (uint8_t)(out + 0.5f);
    }
    // `db`: one SPC1 payload, n bins in FFT order (DC first), captured at `center_mhz`.
    void push_capture(const uint8_t* db, unsigned n, unsigned center_mhz) {
        const int32_t c_khz = (int32_t)center_mhz * 1000, use_khz = 20000, rate_khz = 80000;
        for (unsigned r = 0; r < range_count; r++) {
            const Range& g = ranges[r];
            const int32_t lo = g.lo_mhz * 1000, span = (g.hi_mhz - g.lo_mhz) * 1000, cols = g.x1 - g.x0;
            for (int32_t i = 0; i < cols; i++) {
                int32_t f0 = lo + span * i / cols, f1 = lo + span * (i + 1) / cols, fm = (f0 + f1) / 2;
                int32_t dist = fm > c_khz ? fm - c_khz : c_khz - fm;
                if (dist > use_khz || dist <= cand_khz[g.x0 + i]) continue;
                int32_t k0 = (int32_t)n / 2 + (f0 - c_khz) * (int32_t)n / rate_khz;
                int32_t k1 = (int32_t)n / 2 + (f1 - c_khz) * (int32_t)n / rate_khz;
                if (k1 <= k0) k1 = k0 + 1;
                uint8_t peak = 0;
                for (int32_t k = k0; k < k1; k++) {
                    if (k < 0 || k >= (int32_t)n) continue;
                    unsigned d = (unsigned)(k > (int32_t)n / 2 ? k - (int32_t)n / 2 : (int32_t)n / 2 - k) * 256 / n;
                    uint8_t v = deskirt(db[(k + n / 2) % n], d, center_mhz > 3000);
                    if (v > peak) peak = v;
                }
                cand[g.x0 + i] = peak;
                cand_khz[g.x0 + i] = (int16_t)dist;   // <= 20000 kHz, fits
            }
        }
    }
    // Ends one sweep: tracks the noise floor and adds a waterfall line.
    void commit() {
        for (unsigned x = 0; x < W; x++) {
            if (cand_khz[x] < 0) continue;            // not covered this sweep: keep the last value
            trace[x] = cand[x];
            hold[x] = cand[x] > hold[x] ? cand[x] : (hold[x] > 0 ? hold[x] - 1 : 0);
            cand_khz[x] = -1;
        }
        uint8_t lo = 255;
        for (unsigned x = 0; x < W; x++) if (trace[x] && trace[x] < lo) lo = trace[x];
        // Track the noise floor slowly so the picture does not pump with every burst.
        if (lo != 255) floor_db = lo + 2 < floor_db ? lo + 2 : (floor_db < lo ? floor_db + 1 : floor_db);
        if (fall) {
            fall_head = (fall_head + 1) % FALL_ROWS;
            uint8_t* row = fall + fall_head * FALL_COLS;
            for (unsigned c = 0; c < FALL_COLS; c++) {
                uint8_t v = trace[2 * c] > trace[2 * c + 1] ? trace[2 * c] : trace[2 * c + 1];
                row[c] = level(v);
            }
        }
    }
    void reset() {
        memset(trace, 0, sizeof(trace));
        memset(hold, 0, sizeof(hold));
        memset(sep, 0, sizeof(sep));
        range_count = 0;
        for (unsigned c = 0; c < W; c++) cand_khz[c] = -1;
        fall_head = 0;
        if (fall) memset(fall, 0, FALL_ROWS * FALL_COLS);
    }
    // 0..255 over the display range.
    uint8_t level(uint8_t db) const {
        if (db <= floor_db) return 0;
        unsigned v = (unsigned)(db - floor_db) * 255 / span_db;
        return v > 255 ? 255 : (uint8_t)v;
    }
    // Strong is red, as in WLAN Commander's heatmaps: blue -> cyan -> yellow -> red.
    static uint16_t heat(uint8_t v) {
        unsigned r, g, b;
        if (v < 85) { r = 0; g = v * 3; b = 255; }
        else if (v < 170) { r = (v - 85) * 3; g = 255; b = 255 - (v - 85) * 3; }
        else { r = 255; g = 255 - (v - 170) * 3; b = 0; }
        if (v == 0) { r = g = 0; b = 24; }
        return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    static bool text_on(const char* s, unsigned x, unsigned y) {   // 2x scaled 5x7 font, 12 px cells
        unsigned index = x / 12;
        if (index >= strlen(s) || y < 2 || y >= 16) return false;
        return lcd_glyph(s[index], (x % 12) / 2) & (1 << ((y - 2) / 2));
    }
    uint16_t pixel(unsigned x, unsigned y) const {
        if (y < TITLE_H) return text_on(title, x > 4 ? x - 4 : 999, y) ? 0xffff : 0x1082;
        if (y >= TRACE_Y && y < TRACE_Y + TRACE_H) {
            unsigned from_bottom = TRACE_Y + TRACE_H - 1 - y;
            unsigned t = (unsigned)level(trace[x]) * TRACE_H / 256;
            unsigned h = (unsigned)level(hold[x]) * TRACE_H / 256;
            if (from_bottom == h && h > t) return 0xffff;
            if (sep[x]) return 0x8410;
            if (from_bottom < t) return heat((uint8_t)(from_bottom * 255 / TRACE_H));
            return (from_bottom % 32 == 0) ? 0x2104 : 0;      // faint 32-px grid
        }
        if (y >= AXIS_Y && y < AXIS_Y + 16) return text_on(axis, x > 4 ? x - 4 : 999, y - AXIS_Y) ? 0xc618 : 0;
        if (fall && y >= FALL_Y && y < FALL_Y + FALL_ROWS) {
            unsigned age = y - FALL_Y;                                // newest on top
            const uint8_t* row = fall + ((fall_head + FALL_ROWS - age) % FALL_ROWS) * FALL_COLS;
            return heat(row[x / 2]);
        }
        return 0;
    }
};
