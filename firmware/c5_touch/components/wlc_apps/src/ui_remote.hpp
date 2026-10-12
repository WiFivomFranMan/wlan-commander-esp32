// SPDX-License-Identifier: MIT
#pragma once

namespace wlc::apps {

void ui_remote_init();                         // after the display has started
void ui_remote_tap(int x, int y, int hold_ms);
void ui_remote_swipe(int x0, int y0, int x1, int y1, int ms);
void ui_remote_screenshot();

}
