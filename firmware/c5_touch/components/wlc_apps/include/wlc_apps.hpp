// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include "esp_err.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace wlc::apps {

// Starts the radio owner and the survey task. Call once, before the display.
esp_err_t start_services();

// Every app the launcher shows, in launcher order.
std::vector<esp_brookesia::systems::phone::App *> all();

}
