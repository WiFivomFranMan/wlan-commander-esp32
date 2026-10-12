// SPDX-License-Identifier: MIT
#include "list_view.hpp"
#include "tool_app.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wlc::apps {

lv_obj_t *make_line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int32_t width)
{
    lv_obj_t *label = make_label(parent, "", font, color);
    // LONG_DOT only truncates when the height is fixed; with auto height it wraps instead.
    lv_obj_set_size(label, width, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

void ListView::create(lv_obj_t *parent, Tap on_tap)
{
    on_tap_ = std::move(on_tap);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    empty_ = make_label(parent, "Listening...", &lv_font_montserrat_14, lv_color_hex(0x9fb3c8));
    lv_obj_set_style_pad_all(empty_, 8, 0);
    for (int i = 0; i < kRows; i++) {
        Row &r = rows_[i];
        r.obj = lv_obj_create(parent);
        lv_obj_remove_style_all(r.obj);
        lv_obj_set_size(r.obj, LV_PCT(100), 46);
        lv_obj_set_style_bg_color(r.obj, lv_color_hex(0x1a222b), 0);
        lv_obj_set_style_bg_color(r.obj, lv_color_hex(0x2a3846), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r.obj, 6, 0);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);

        r.badge = lv_obj_create(r.obj);
        lv_obj_remove_style_all(r.badge);
        lv_obj_set_size(r.badge, 40, 38);
        lv_obj_align(r.badge, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_opa(r.badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r.badge, 5, 0);
        lv_obj_remove_flag(r.badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *num = make_label(r.badge, "", &lv_font_montserrat_16, lv_color_black());
        lv_obj_center(num);

        r.title = make_line(r.obj, &lv_font_montserrat_16, lv_color_white(), 130);
        lv_obj_align(r.title, LV_ALIGN_TOP_LEFT, 52, 4);
        r.tag = make_label(r.obj, "", &lv_font_montserrat_12, lv_color_hex(0x7fc4ff));
        lv_obj_align(r.tag, LV_ALIGN_TOP_RIGHT, -6, 6);
        r.sub = make_line(r.obj, &lv_font_montserrat_12, lv_color_hex(0x9fb3c8), 176);
        lv_obj_align(r.sub, LV_ALIGN_BOTTOM_LEFT, 52, -5);
        lv_obj_add_event_cb(r.obj, [](lv_event_t *e) {
            auto *self = static_cast<ListView *>(lv_event_get_user_data(e));
            lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
            for (int k = 0; k < kRows; k++) if (self->rows_[k].obj == target && self->on_tap_) self->on_tap_(k);
        }, LV_EVENT_CLICKED, this);
    }
}

void ListView::set_empty_text(const char *text) { if (empty_) lv_label_set_text(empty_, text); }

void ListView::set_count(int count)
{
    if (count > kRows) count = kRows;
    for (int i = 0; i < kRows; i++) {
        if (i < count) lv_obj_remove_flag(rows_[i].obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(rows_[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (count) lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN);
}

void ListView::set_row(int i, const char *title, const char *tag, const char *subtitle, const char *badge,
                       uint32_t badge_color)
{
    if (i < 0 || i >= kRows) return;
    Row &r = rows_[i];
    lv_label_set_text(r.title, title);
    lv_label_set_text(r.tag, tag);
    lv_label_set_text(r.sub, subtitle);
    lv_label_set_text(lv_obj_get_child(r.badge, 0), badge);
    lv_obj_set_style_bg_color(r.badge, lv_color_hex(badge_color), 0);
}

void DetailList::section(const char *title)
{
    if (count >= kMax) return;
    DetailEntry &e = items[count++];
    snprintf(e.key, sizeof(e.key), "%s", title);
    e.value[0] = 0;
    e.section = true;
}

void DetailList::field(const char *key, const char *fmt, ...)
{
    if (count >= kMax) return;
    DetailEntry &e = items[count++];
    snprintf(e.key, sizeof(e.key), "%s", key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.value, sizeof(e.value), fmt, ap);
    va_end(ap);
    e.section = false;
}

void DetailView::open(lv_obj_t *screen, const char *heading)
{
    close();
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *top = lv_obj_create(root_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x1c2630), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_t *done = lv_button_create(top);
    lv_obj_set_size(done, 32, 28);
    lv_obj_align(done, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x2c3a48), 0);
    lv_obj_center(make_label(done, LV_SYMBOL_LEFT, &lv_font_montserrat_14, lv_color_white()));
    lv_obj_add_event_cb(done, [](lv_event_t *e) {
        static_cast<DetailView *>(lv_event_get_user_data(e))->close();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *head = make_line(top, &lv_font_montserrat_16, lv_color_white(), 186);
    lv_label_set_text(head, heading);
    lv_obj_align(head, LV_ALIGN_LEFT_MID, 46, 0);

    rows_ = lv_obj_create(root_);
    lv_obj_remove_style_all(rows_);
    lv_obj_set_width(rows_, LV_PCT(100));
    lv_obj_set_flex_grow(rows_, 1);
    lv_obj_set_flex_flow(rows_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(rows_, 8, 0);
    lv_obj_set_style_pad_bottom(rows_, 12, 0);
    lv_obj_set_style_pad_row(rows_, 3, 0);
    lv_obj_add_flag(rows_, LV_OBJ_FLAG_SCROLLABLE);
    count_ = -1;
}

void DetailView::show(const DetailList &list)
{
    if (!rows_) return;
    bool same = list.count == count_;
    for (int i = 0; same && i < list.count; i++) same = !strcmp(layout_[i], list.items[i].key);
    if (!same) {
        lv_obj_clean(rows_);
        count_ = list.count;
        for (int i = 0; i < list.count; i++) {
            const DetailEntry &e = list.items[i];
            snprintf(layout_[i], sizeof(layout_[i]), "%s", e.key);
            values_[i] = nullptr;
            // Plain labels only: nothing on this page is a link or a control. ANQP emergency
            // elements (259/269/271) in particular are shown as text and never made actionable.
            if (e.section) {
                lv_obj_t *h = make_label(rows_, e.key, &lv_font_montserrat_12, lv_color_hex(0x7fc4ff));
                lv_obj_set_style_pad_top(h, 8, 0);
                continue;
            }
            lv_obj_t *row = lv_obj_create(rows_);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_t *k = make_label(row, e.key, &lv_font_montserrat_12, lv_color_hex(0x8a9aab));
            lv_obj_set_width(k, 74);
            lv_obj_set_style_pad_top(k, 2, 0);
            lv_obj_t *v = make_label(row, e.value, &lv_font_montserrat_14, lv_color_hex(0xe6edf3));
            lv_obj_set_width(v, 150);
            lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
            lv_obj_set_x(v, 78);
            values_[i] = v;
        }
        return;
    }
    for (int i = 0; i < list.count; i++)
        if (values_[i] && strcmp(lv_label_get_text(values_[i]), list.items[i].value))
            lv_label_set_text(values_[i], list.items[i].value);
}

void DetailView::close()
{
    if (root_) lv_obj_delete(root_);
    root_ = rows_ = nullptr;
    count_ = -1;
}

}
