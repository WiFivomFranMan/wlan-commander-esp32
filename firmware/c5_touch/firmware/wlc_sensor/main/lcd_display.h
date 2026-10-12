// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lcd_model.h"
#include <stddef.h>
bool wlc_lcd_init();
void wlc_lcd_show_mode(LcdSnapshot::Mode mode);
void wlc_lcd_tick(const LcdSnapshot& snapshot);
bool wlc_lcd_identify(unsigned number, unsigned seconds);
bool wlc_lcd_led_override(uint32_t now);
void wlc_lcd_info(char* output, size_t capacity);

// Local spectrum: the sensor image draws ESP-SDR's frames itself instead of streaming them.
bool wlc_lcd_spectrum_begin();                       // allocates the waterfall; false = no display
void wlc_lcd_spectrum_ranges(const uint16_t (*ranges)[2], unsigned count);   // after begin
void wlc_lcd_spectrum_frame(const uint8_t* db, unsigned bins, unsigned center_mhz);
void wlc_lcd_spectrum_commit();                        // a full sweep is in: waterfall line
void wlc_lcd_spectrum_draw(const char* title, const char* axis);
void wlc_lcd_spectrum_end();

#include "wlc_generator.hpp"
bool wlc_lcd_generator_begin();
bool wlc_lcd_generator_healthy();
wlc::generator::Action wlc_lcd_generator_input();
void wlc_lcd_generator_tick(const wlc::generator::Run &run, uint32_t session_ms);
bool wlc_lcd_generator_tap(int x, int y); // virtual landscape tap, same hit targets
void wlc_lcd_generator_cancel_input();
bool wlc_lcd_generator_shot();            // queued, streamed without waiting on USB
void wlc_lcd_generator_shot_tick();
bool wlc_lcd_generator_shot_cancel();
