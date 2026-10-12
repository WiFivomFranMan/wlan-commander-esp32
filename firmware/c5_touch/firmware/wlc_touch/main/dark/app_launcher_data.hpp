/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/widgets/app_launcher/esp_brookesia_app_launcher.hpp"

namespace esp_brookesia::systems::phone {

constexpr AppLauncherIcon::Data STYLESHEET_240_320_DARK_APP_LAUNCHER_ICON_DATA = {
    .main = {
        .size = gui::StyleSize::SQUARE(72),
        .layout_row_pad = 6,
    },
    .image = {
        .default_size = gui::StyleSize::SQUARE(48),
        .press_size = gui::StyleSize::SQUARE(42),
    },
    .label = {
        .text_font = gui::StyleFont::SIZE(14),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
};

constexpr AppLauncherData STYLESHEET_240_320_DARK_APP_LAUNCHER_DATA = {
    .main = {
        .y_start = 0,
        .size = gui::StyleSize::RECT_PERCENT(100, 100),
    },
    .table = {
        .default_num = 1,
        .size = gui::StyleSize::RECT_PERCENT(90, 90),
    },
    .indicator = {
        .main_size = gui::StyleSize::RECT_W_PERCENT(100, 20),
        .main_layout_column_pad = 30,
        .main_layout_bottom_offset = 4,
        .spot_inactive_size = gui::StyleSize::SQUARE(4),
        .spot_active_size = gui::StyleSize::RECT(16, 4),
        .spot_inactive_background_color = gui::StyleColor::COLOR(0xC6C6C6),
        .spot_active_background_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .icon = STYLESHEET_240_320_DARK_APP_LAUNCHER_ICON_DATA,
    .flags = {
        .enable_table_scroll_anim = 0,
    },
};

} // namespace esp_brookesia::systems::phone
