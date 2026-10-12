// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include "lvgl.h"

namespace wlc::apps {

// Full-screen page for choosing the swept channel groups and dwell. `on_done` runs after the
// settings are saved, so the caller can restart the radio with the new plan.
class ChannelPicker {
public:
    void open(lv_obj_t *screen, std::function<void()> on_done);
    void close();
    bool is_open() const { return root_ != nullptr; }

private:
    void refresh_summary();
    lv_obj_t *root_ = nullptr, *summary_ = nullptr, *all24_ = nullptr, *dwell_ = nullptr;
    lv_obj_t *switches_[5] = {};
    std::function<void()> on_done_;
};

}
