// SPDX-License-Identifier: MIT
// Sensor and Spectrum live in the other image (ota_1): the WLAN Commander combined C5 firmware,
// whose BLE protocol the phone apps already speak and whose ESP-SDR spectrum needs the RF scratch
// layout this image does not have. These tiles explain that and reboot into it.
#include "switch_app.hpp"

#include <cstdio>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"

namespace wlc::apps {

SwitchApp::SwitchApp(const char *name, const char *what, bool spectrum)
    : ToolApp(name, spectrum ? &wlc_icon_spectrum : &wlc_icon_sensor), what_(what), spectrum_(spectrum) {}

static const esp_partition_t *sensor_slot(esp_app_desc_t *desc)
{
    const esp_partition_t *slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!slot || esp_ota_get_partition_description(slot, desc) != ESP_OK) return nullptr;
    return slot;
}

bool boot_sensor_image(int spectrum_view)
{
    esp_app_desc_t desc;
    const esp_partition_t *slot = sensor_slot(&desc);
    if (!slot) return false;
    // The sensor image reads and clears this flag at boot (shared NVS partition).
    nvs_handle_t h;
    if (nvs_open("wlc", NVS_READWRITE, &h) != ESP_OK) return false;
    auto erase = [h](const char *key) {
        esp_err_t err = nvs_erase_key(h, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    };
    esp_err_t err = erase("boot_generator");
    if (err == ESP_OK) err = erase("rf_shield_ack");
    if (err == ESP_OK) err = spectrum_view >= 0
        ? nvs_set_u8(h, "boot_spectrum", (uint8_t)(spectrum_view + 1)) : erase("boot_spectrum");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (esp_ota_set_boot_partition(slot) != ESP_OK) return false;
    esp_restart();
}

void SwitchApp::build(lv_obj_t *body)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *text = make_label(body, what_, &lv_font_montserrat_14, lv_color_hex(0xd8e2ec));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text, LV_PCT(100));

    esp_app_desc_t desc;
    if (!sensor_slot(&desc)) {
        make_label(body, "Sensor image is not installed in ota_1.", &lv_font_montserrat_14,
                   lv_color_hex(0xff8a65));
        return;
    }
    lv_obj_t *hint = make_label(body, spectrum_ ? "Press BOOT for the next view; hold it 2 s to come back here."
                                                : "Hold BOOT for 2 s to come back here.",
                                &lv_font_montserrat_12, lv_color_hex(0x9fb3c8));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    // Must match SPECTRUM_BANDS in firmware/wlc_sensor/main/combined.cpp.
    static const char *kViews[] = {"2.4 GHz", "5 GHz  ch 36-64", "5 GHz  ch 100-144", "5 GHz  ch 149-165",
                                   "5 GHz  all channels"};
    const int buttons = spectrum_ ? 5 : 1;
    for (int i = 0; i < buttons; i++) {
        lv_obj_t *go = lv_button_create(body);
        lv_obj_set_size(go, LV_PCT(100), spectrum_ ? 34 : 44);
        lv_obj_set_style_bg_color(go, lv_color_hex(spectrum_ ? 0x1e88e5 : 0x2e7d32), 0);
        lv_obj_center(make_label(go, spectrum_ ? kViews[i] : "Start sensor", &lv_font_montserrat_14, lv_color_white()));
        lv_obj_set_user_data(go, (void *)(intptr_t)(spectrum_ ? i : -1));
        lv_obj_add_event_cb(go, [](lv_event_t *e) {
            auto *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
            boot_sensor_image((int)(intptr_t)lv_obj_get_user_data(b));
        }, LV_EVENT_CLICKED, nullptr);
    }
}

}
