// SPDX-License-Identifier: MIT
#include "channel_picker.hpp"

#include <cstdio>
#include "channel_plan.hpp"
#include "list_view.hpp"
#include "tool_app.hpp"

namespace wlc::apps {
namespace {

const uint16_t kDwells[] = {150, 250, 500, 1000};
const char *kDwellMap[] = {"150", "250", "500", "1000 ms", ""};
const char *k24Map[] = {"1, 6, 11", "1 - 11", ""};

lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = make_label(parent, text, &lv_font_montserrat_12, lv_color_hex(0x7fc4ff));
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

void style_matrix(lv_obj_t *m)
{
    lv_obj_set_size(m, LV_PCT(100), 34);
    lv_obj_set_style_pad_all(m, 2, 0);
    lv_obj_set_style_pad_gap(m, 3, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x1a222b), 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x2c3a48), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x1e88e5), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
}

}  // namespace

void ChannelPicker::open(lv_obj_t *screen, std::function<void()> on_done)
{
    close();
    on_done_ = std::move(on_done);
    const ChannelSettings &s = channel_settings();

    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_pad_row(root_, 4, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(root_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), 32);
    make_label(top, "Channels", &lv_font_montserrat_16, lv_color_white());
    lv_obj_t *done = lv_button_create(top);
    lv_obj_set_size(done, 64, 30);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x2e7d32), 0);
    lv_obj_center(make_label(done, "Done", &lv_font_montserrat_14, lv_color_white()));
    lv_obj_add_event_cb(done, [](lv_event_t *e) {
        auto *self = static_cast<ChannelPicker *>(lv_event_get_user_data(e));
        save_channel_settings();
        auto cb = self->on_done_;
        self->close();
        if (cb) cb();
    }, LV_EVENT_CLICKED, this);

    summary_ = make_label(root_, "", &lv_font_montserrat_12, lv_color_hex(0x9fb3c8));

    section(root_, "BANDS");
    for (int i = 0; i < 5; i++) {
        lv_obj_t *row = lv_obj_create(root_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1a222b), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_t *name = make_line(row, &lv_font_montserrat_14, lv_color_white(), 150);
        lv_label_set_text(name, kGroups[i].name);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 8, 2);
        lv_obj_t *chans = make_line(row, &lv_font_montserrat_12, lv_color_hex(0x9fb3c8), 150);
        lv_label_set_text(chans, kGroups[i].channels);
        lv_obj_align(chans, LV_ALIGN_BOTTOM_LEFT, 8, -2);
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 46, 24);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
        if (s.groups & kGroups[i].bit) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_user_data(sw, (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            auto *self = static_cast<ChannelPicker *>(lv_event_get_user_data(e));
            lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(sw);
            ChannelSettings &cs = channel_settings();
            if (lv_obj_has_state(sw, LV_STATE_CHECKED)) cs.groups |= kGroups[i].bit;
            else cs.groups &= ~kGroups[i].bit;
            self->refresh_summary();
        }, LV_EVENT_VALUE_CHANGED, this);
        switches_[i] = sw;
    }

    section(root_, "2.4 GHZ CHANNELS");
    all24_ = lv_buttonmatrix_create(root_);
    lv_buttonmatrix_set_map(all24_, k24Map);
    style_matrix(all24_);
    lv_buttonmatrix_set_button_ctrl(all24_, s.all_24 ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(all24_, [](lv_event_t *e) {
        auto *self = static_cast<ChannelPicker *>(lv_event_get_user_data(e));
        channel_settings().all_24 = lv_buttonmatrix_get_selected_button(self->all24_) == 1;
        self->refresh_summary();
    }, LV_EVENT_VALUE_CHANGED, this);

    section(root_, "DWELL PER CHANNEL");
    dwell_ = lv_buttonmatrix_create(root_);
    lv_buttonmatrix_set_map(dwell_, kDwellMap);
    style_matrix(dwell_);
    int selected = 1;
    for (int i = 0; i < 4; i++) if (kDwells[i] == s.dwell_ms) selected = i;
    lv_buttonmatrix_set_button_ctrl(dwell_, selected, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(dwell_, [](lv_event_t *e) {
        auto *self = static_cast<ChannelPicker *>(lv_event_get_user_data(e));
        uint32_t b = lv_buttonmatrix_get_selected_button(self->dwell_);
        if (b < 4) channel_settings().dwell_ms = kDwells[b];
        self->refresh_summary();
    }, LV_EVENT_VALUE_CHANGED, this);

    refresh_summary();
}

void ChannelPicker::refresh_summary()
{
    if (!summary_) return;
    radio::Plan plan;
    build_plan(plan, false, 0);
    char text[64];
    unsigned ms = plan.channel_count * plan.dwell_ms;
    snprintf(text, sizeof(text), "%u channels, one sweep every %u.%u s", plan.channel_count, ms / 1000, (ms % 1000) / 100);
    lv_label_set_text(summary_, text);
}

void ChannelPicker::close()
{
    if (root_) lv_obj_delete(root_);
    root_ = summary_ = all24_ = dwell_ = nullptr;
    for (auto &s : switches_) s = nullptr;
}

}
