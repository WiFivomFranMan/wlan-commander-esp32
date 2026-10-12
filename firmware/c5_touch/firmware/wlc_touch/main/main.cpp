// SPDX-License-Identifier: MIT
// WLC Touch: the ESP-Brookesia phone shell with WLAN Commander's tools as its only apps.
// Derived from Waveshare's 08_bookesia example (Apache-2.0); factory apps removed on purpose —
// the AI-chat and weather apps join Wi-Fi as a station, which fights promiscuous capture.
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "wlc_apps.hpp"
#include "ui_remote.hpp"
#include "./dark/stylesheet.hpp"

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

static const char *TAG = "wlc_touch";

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Radio first: Wi-Fi's internal buffers must come out of SRAM before LVGL fragments it.
    // A radio failure must not take the launcher down with it; the apps report it instead.
    if (wlc::apps::start_services() != ESP_OK) ESP_LOGE(TAG, "radio services failed to start");

    if (!bsp_display_start()) { ESP_LOGE(TAG, "display start failed"); return; }
    bsp_display_backlight_on();

    LvLock::registerCallbacks([](int timeout_ms) {
        return bsp_display_lock(timeout_ms) == ESP_OK;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    Phone *phone = new Phone();
    Stylesheet *stylesheet = new Stylesheet(STYLESHEET_240_320_DARK);
    phone->addStylesheet(stylesheet);
    phone->activateStylesheet(stylesheet);
    delete stylesheet;

    wlc::apps::ui_remote_init();

    LvLockGuard gui_guard;
    if (!phone->begin()) { ESP_LOGE(TAG, "phone begin failed"); return; }
    for (auto *app : wlc::apps::all()) {
        if (!phone->installApp(app)) ESP_LOGE(TAG, "install %s failed", app->getName());
    }
}
