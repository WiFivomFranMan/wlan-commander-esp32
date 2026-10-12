// SPDX-License-Identifier: MIT
#pragma once
#include "tool_app.hpp"

namespace wlc::apps {

// Boots the sensor image: as a BLE sensor (spectrum_view < 0) or drawing spectrum view N.
bool boot_sensor_image(int spectrum_view);

class SwitchApp : public ToolApp {
public:
    SwitchApp(const char *name, const char *what, bool spectrum);
protected:
    void build(lv_obj_t *body) override;
private:
    const char *what_;
    bool spectrum_;
};

}
