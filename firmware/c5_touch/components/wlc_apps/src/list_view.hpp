// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include "lvgl.h"

namespace wlc::apps {

// A fixed pool of list rows updated in place. Rebuilding LVGL objects twice a second would
// churn the heap; changing label text does not.
//
//   [-52]  SSID ......................  ch 36
//          40 MHz  WPA3  Wi-Fi 6  5 GHz
class ListView {
public:
    static constexpr int kRows = 30;
    using Tap = std::function<void(int index)>;

    void create(lv_obj_t *parent, Tap on_tap);
    void set_count(int count);
    void set_row(int i, const char *title, const char *tag, const char *subtitle, const char *badge,
                 uint32_t badge_color);
    void set_empty_text(const char *text);

private:
    struct Row { lv_obj_t *obj, *title, *tag, *sub, *badge; };
    Row rows_[kRows] = {};
    lv_obj_t *empty_ = nullptr;
    Tap on_tap_;
};

// Rows for a detail page: section headings and key/value fields.
struct DetailEntry {
    char key[20];
    char value[100];
    bool section;
};

struct DetailList {
    static constexpr int kMax = 64;
    DetailEntry *items;   // caller-owned, kMax entries
    int count = 0;
    void section(const char *title);
    void field(const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
};

// Full-screen page over the app, closed by its own button. Rows are updated in place while the
// layout (same keys in the same order) is unchanged, and rebuilt only when it changes.
class DetailView {
public:
    void open(lv_obj_t *screen, const char *heading);
    void show(const DetailList &list);
    void close();
    bool is_open() const { return root_ != nullptr; }
private:
    lv_obj_t *root_ = nullptr, *rows_ = nullptr;
    char layout_[DetailList::kMax][20] = {};
    lv_obj_t *values_[DetailList::kMax] = {};
    int count_ = -1;
};

// A single-line label of fixed width that ends in "..." instead of wrapping.
lv_obj_t *make_line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int32_t width);

}
