// SPDX-License-Identifier: MIT
#include "tool_app.hpp"

namespace wlc::apps {

ToolApp::ToolApp(const char *name, const void *icon)
    : App(name, icon, true, false, false) {}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

bool ToolApp::run()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_row(screen, 0, 0);

    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c2630), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_button_create(bar);
    lv_obj_set_size(back_btn, 32, 28);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2c3a48), 0);
    lv_obj_t *arrow = make_label(back_btn, LV_SYMBOL_LEFT, &lv_font_montserrat_14, lv_color_white());
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        static_cast<ToolApp *>(lv_event_get_user_data(e))->notifyCoreClosed();
    }, LV_EVENT_CLICKED, this);

    title_ = make_label(bar, getName(), &lv_font_montserrat_16, lv_color_white());
    lv_obj_align(title_, LV_ALIGN_LEFT_MID, 40, 0);
    status_ = make_label(bar, "", &lv_font_montserrat_12, lv_color_hex(0x9fb3c8));
    lv_obj_align(status_, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 4, 0);
    build(body);

    timer_ = lv_timer_create([](lv_timer_t *t) {
        static_cast<ToolApp *>(lv_timer_get_user_data(t))->refresh();
    }, 500, this);
    return true;
}

bool ToolApp::back() { return notifyCoreClosed(); }

bool ToolApp::close()
{
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    teardown();
    title_ = status_ = nullptr;
    return true;
}

}
