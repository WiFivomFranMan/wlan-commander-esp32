// SPDX-License-Identifier: MIT
#pragma once
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace wlc::apps {

// Shared shape of every tool: a title row with a back control, and a body the tool fills.
// The edge-swipe gesture also goes back; the button is there because a swipe on a 2.8"
// screen in a hand is easy to miss.
class ToolApp : public esp_brookesia::systems::phone::App {
public:
    ToolApp(const char *name, const void *icon);

protected:
    bool run() override;
    bool back() override;
    bool close() override;

    virtual void build(lv_obj_t *body) = 0;
    virtual void refresh() {}          // every 500 ms while open, under the LVGL lock
    virtual void teardown() {}

    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;       // right side of the title row

private:
    lv_timer_t *timer_ = nullptr;
};

extern "C" {
extern const lv_image_dsc_t wlc_icon_scan, wlc_icon_clients, wlc_icon_passpoint, wlc_icon_spectrum,
    wlc_icon_sensor, wlc_icon_airhorn;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

}
