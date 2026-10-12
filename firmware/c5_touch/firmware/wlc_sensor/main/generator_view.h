// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wlc_generator.hpp"
#include "lcd_raster.h"

struct GeneratorView {
    LcdFrame frame;
    bool start_enabled = false;
    void update(const wlc::generator::Run &run, uint32_t now, bool touch, uint32_t session_ms = UINT32_MAX) {
        using namespace wlc::generator;
        const Config &c = run.config;
        frame = LcdFrame{};
        start_enabled = session_ms >= total_ms(c) + (c.workflow == Radar ? 1000 : 0) && touch && !run.running() && run.phase != Phase::Error;
        frame.accent = run.phase == Phase::Active ? 0xf800 : run.phase == Phase::Gap ? 0xffe0 : 0x07e0;
        snprintf(frame.rows[0], 27, "AIRHORN  RF GENERATOR");
        snprintf(frame.rows[1], 27, "%s", !session_ms ? "RF SESSION LOCKED" : phase_name(run.phase));
        snprintf(frame.rows[2], 27, "%s  CH %lu", c.tone ? "CW TONE" : "PACKET 6M", (unsigned long)channel_at(c, run.index));
        snprintf(frame.rows[3], 27, "ATTENUATION %lu.%02lu DB", (unsigned long)(c.backoff_qdb % 81) / 4, (unsigned long)(c.backoff_qdb % 4) * 25);
        snprintf(frame.rows[4], 27, "%s", c.workflow == Radar ? kRadar[c.radar_profile].name : c.workflow == Single ? "SINGLE CHANNEL" : c.workflow == Pulse ? "FINITE PULSES" : c.band5 ? "TRAVERSE 5 GHZ" : "TRAVERSE 2.4 GHZ");
        snprintf(frame.rows[5], 27, "BURST %lu/%lu  %lu MS", (unsigned long)(run.index % 100) + 1, (unsigned long)(steps(c) % 101), (unsigned long)(duration(c) % 30001));
        if (c.workflow == Radar) snprintf(frame.rows[5], 27, "%.3fUS  %lu PULSES", kRadar[c.radar_profile].width_ns / 1000.0, (unsigned long)(kRadar[c.radar_profile].pulses % 103));
        if (run.running()) snprintf(frame.rows[6], 27, "%s %lu MS", run.phase == Phase::Gap ? "GAP REMAIN" : "RF REMAIN", (unsigned long)run.remaining(now));
        else snprintf(frame.rows[6], 27, "TOTAL %luS OFF %luMS", (unsigned long)(total_ms(c) / 1000 % 6001), (unsigned long)(c.workflow == Pulse ? c.off_ms % 30001 : 0));
        snprintf(frame.rows[7], 27, "%s", !touch ? "TOUCH/DISPLAY UNAVAILABLE" : c.workflow == Radar ? "RF TIMING UNVERIFIED" : run.running() ? "STOP CANCELS ENTIRE RUN" : "POWER UNCALIBRATED");
        if (!session_ms) snprintf(frame.rows[7], 27, "RETURN TO ACK SHIELDING");
        else if (!run.running() && session_ms != UINT32_MAX) snprintf(frame.rows[6], 27, "SESSION LEFT %luS", (unsigned long)(session_ms/1000));
        snprintf(frame.rows[8], 27, "HOLD BOOT 2S TO RETURN");
        frame.finish();
    }
    uint16_t pixel(unsigned x, unsigned y) const {
        if (y < 184 || y >= 236 || x < 4 || x >= 316) return lcd_pixel(frame, x, y);
        unsigned third = x * 3 / 320;
        unsigned left = third * 320 / 3, right = (third + 1) * 320 / 3;
        if (x < left + 4 || x >= right - 4) return 0;
        uint16_t bg = third == 0 ? (start_enabled ? 0x03e0 : 0x2104) : third == 1 ? 0xb800 : 0x041f;
        const char *text = third == 0 ? "START" : third == 1 ? "STOP" : "RETURN";
        unsigned n = strlen(text), tx = left + (right - left - n * 12) / 2;
        if (x >= tx && x < tx + n * 12 && y >= 202 && y < 216 &&
            (lcd_glyph(text[(x-tx)/12], ((x-tx)%12)/2) & (1 << ((y-202)/2)))) return 0xffff;
        return bg;
    }
};
