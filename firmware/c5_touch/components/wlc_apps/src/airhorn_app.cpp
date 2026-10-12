// SPDX-License-Identifier: MIT
#include "airhorn_app.hpp"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "rf_approval.hpp"
#include "esp_mac.h"
#include <cstdio>
#include <cstring>

namespace wlc::apps {
namespace {
bool save(const generator::Config &c, bool boot) {
    nvs_handle_t h;
    if (!generator::valid(c) || nvs_open("wlc", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "gen_config", &c, sizeof(c));
    if (err == ESP_OK && boot) err = nvs_set_u8(h, "boot_generator", generator::kVersion);
    if (err == ESP_OK && boot) err = nvs_set_u8(h, "rf_shield_ack", 1);
    if (err == ESP_OK && boot) {
        esp_err_t clear = nvs_erase_key(h, "boot_spectrum");
        if (clear != ESP_OK && clear != ESP_ERR_NVS_NOT_FOUND) err = clear;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h); return err == ESP_OK;
}
}
generator::Config generator_config() {
    generator::Config config, loaded;
    nvs_handle_t h;
    if (nvs_open("wlc", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(loaded);
        if (nvs_get_blob(h, "gen_config", &loaded, &n) == ESP_OK) {
            if (n == 44 && loaded.version == 1) loaded.version = generator::kVersion;
            if ((n == 44 || n == sizeof(loaded)) && generator::valid(loaded)) config = loaded;
        }
        nvs_close(h);
    }
    return config;
}
bool save_generator_config(const generator::Config &config) { return save(config, false); }
bool boot_generator_image(const generator::Config &c, bool shielded_ack) {
    if (!shielded_ack || !rf::approved() || generator::total_ms(c) > rf::kMaxSessionMs) return false;
    const auto *slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    esp_app_desc_t desc{};
    if (!slot || esp_ota_get_partition_description(slot, &desc) != ESP_OK ||
        strcmp(desc.project_name, "wlc_sensor") || strcmp(desc.version, generator::kImageVersion) || !save(c, true)) return false;
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        nvs_handle_t h;
        if (nvs_open("wlc", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, "boot_generator"); nvs_erase_key(h, "rf_shield_ack"); nvs_commit(h); nvs_close(h); }
        return false;
    }
    esp_restart();
}
AirhornApp::AirhornApp() : ToolApp("AirHORN", &wlc_icon_airhorn) {}
void AirhornApp::build(lv_obj_t *body) {
    config_ = generator_config();
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    approved_ = rf::approved();
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char gate[600];
    snprintf(gate, sizeof(gate), "%s\n\n%s\nApproval: pcapai.app/rf-approval/\nBoard: %02x:%02x:%02x:%02x:%02x:%02x\nSessions expire after 15 minutes. A reboot locks RF again.", approved_ ? "Device approved. RF is locked until you acknowledge shielding." : "RF LOCKED: web admin device approval required.", rf::kWarning, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    auto *warning = make_label(body, gate, &lv_font_montserrat_12, lv_color_hex(0xffcc80));
    lv_obj_set_width(warning, LV_PCT(100)); lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    ack_ = lv_checkbox_create(body);
    lv_obj_set_width(ack_, LV_PCT(100));
    lv_checkbox_set_text(ack_, "I confirm RF containment");
    lv_obj_set_style_text_font(ack_, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(ack_, lv_color_hex(0xd8e2ec), LV_PART_MAIN);
    lv_obj_add_event_cb(ack_, [](lv_event_t *e) { static_cast<AirhornApp *>(lv_event_get_user_data(e))->changed(); }, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    // Numeric spinboxes use explicit +/- buttons so no keyboard covers the screen.
    const char *names[] = {"Workflow", "Waveform", "Channel", "Traversal band", "Traversal speed", "Attenuation (0.25 dB)", "Single burst (ms)", "Pulse on (ms)", "Pulse off (ms)", "Pulse count", "Radar event pattern"};
    const char *options[] = {"Single\nTraverse\nPulse\nDFS radar", "Packet\nCW tone", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n36\n40\n44\n48\n52\n56\n60\n64\n100\n104\n108\n112\n116\n120\n124\n128\n132\n136\n140\n144\n149\n153\n157\n161\n165", "2.4 GHz\n5 GHz", "Fast: 5 s\nSlow: 30 s"};
    uint32_t selected[] = {config_.workflow, config_.tone, 0, config_.band5, config_.dwell_ms == 30000 ? 1u : 0u};
    for (unsigned j = 0; j < sizeof(generator::kChannels) / sizeof(generator::kChannels[0]); ++j) if (generator::kChannels[j] == config_.channel) selected[2] = j;
    for (int i = 0; i < 11; ++i) {
        rows_[i] = lv_obj_create(body);
        lv_obj_remove_style_all(rows_[i]);
        lv_obj_set_size(rows_[i], LV_PCT(100), (i < 5 || i == 10) ? 68 : 76);
        make_label(rows_[i], names[i], &lv_font_montserrat_12, lv_color_hex(0x9fb3c8));
        if (i < 5 || i == 10) {
            fields_[i] = lv_dropdown_create(rows_[i]);
            if (i == 10) {
                lv_dropdown_clear_options(fields_[i]);
                for (const auto &p : generator::kRadar) lv_dropdown_add_option(fields_[i], p.name, LV_DROPDOWN_POS_LAST);
                lv_dropdown_set_selected(fields_[i], config_.radar_profile);
            } else {
                lv_dropdown_set_options(fields_[i], options[i]);
                lv_dropdown_set_selected(fields_[i], selected[i]);
            }
            lv_obj_set_size(fields_[i], LV_PCT(100), 38);
            lv_obj_align(fields_[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        } else {
            fields_[i] = lv_spinbox_create(rows_[i]);
            lv_spinbox_set_range(fields_[i], i == 5 ? 0 : 1, i == 5 ? 80 : i == 9 ? 100 : 30000);
            lv_spinbox_set_digit_format(fields_[i], i == 5 ? 2 : i == 9 ? 3 : 5, 0);
            lv_spinbox_set_step(fields_[i], 1);
            lv_spinbox_set_value(fields_[i], i == 5 ? config_.backoff_qdb : i == 6 ? config_.single_ms : i == 7 ? config_.on_ms : i == 8 ? config_.off_ms : config_.count);
            lv_obj_set_size(fields_[i], 120, 44);
            lv_obj_align(fields_[i], LV_ALIGN_BOTTOM_MID, 0, 0);
            for (int plus = 0; plus < 2; ++plus) {
                auto *b = lv_button_create(rows_[i]); lv_obj_set_size(b, 44, 44);
                lv_obj_align(b, plus ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, 0, 0);
                lv_obj_center(make_label(b, plus ? "+" : "-", &lv_font_montserrat_16, lv_color_white()));
                lv_obj_set_user_data(b, (void *)(intptr_t)plus);
                lv_obj_add_event_cb(b, [](lv_event_t *e) {
                    auto *sb = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
                    auto *b = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
                    if (lv_obj_get_user_data(b)) lv_spinbox_increment(sb); else lv_spinbox_decrement(sb);
                }, LV_EVENT_CLICKED, fields_[i]);
            }
        }
        lv_obj_add_event_cb(fields_[i], [](lv_event_t *e) { static_cast<AirhornApp *>(lv_event_get_user_data(e))->changed(); }, LV_EVENT_VALUE_CHANGED, this);
    }
    note_ = make_label(body, "", &lv_font_montserrat_12, lv_color_hex(0xffcc80));
    lv_obj_set_width(note_, LV_PCT(100)); lv_label_set_long_mode(note_, LV_LABEL_LONG_WRAP);
    auto *go = go_ = lv_button_create(body); lv_obj_set_size(go, LV_PCT(100), 46);
    lv_obj_center(make_label(go, "Open generator", &lv_font_montserrat_14, lv_color_white()));
    lv_obj_add_event_cb(go, [](lv_event_t *e) {
        auto *self = static_cast<AirhornApp *>(lv_event_get_user_data(e));
        if (!boot_generator_image(self->config_, lv_obj_has_state(self->ack_, LV_STATE_CHECKED))) lv_label_set_text(self->note_, "Unable to open generator. Install the matching AirHORN sensor image and check saved settings.");
    }, LV_EVENT_CLICKED, this);
    changed();
}
void AirhornApp::changed() {
    if (!note_) return;
    config_.workflow = lv_dropdown_get_selected(fields_[0]); config_.tone = lv_dropdown_get_selected(fields_[1]);
    config_.channel = generator::kChannels[lv_dropdown_get_selected(fields_[2])];
    config_.band5 = lv_dropdown_get_selected(fields_[3]); config_.dwell_ms = lv_dropdown_get_selected(fields_[4]) ? 30000 : 5000;
    config_.backoff_qdb = lv_spinbox_get_value(fields_[5]); config_.single_ms = lv_spinbox_get_value(fields_[6]); config_.on_ms = lv_spinbox_get_value(fields_[7]);
    config_.off_ms = lv_spinbox_get_value(fields_[8]); config_.count = lv_spinbox_get_value(fields_[9]);
    config_.radar_profile = lv_dropdown_get_selected(fields_[10]);
    if (config_.workflow == generator::Radar) {
        config_.tone = 1; lv_dropdown_set_selected(fields_[1], 1);
        if (!generator::dfs_channel(config_.channel)) {
            config_.channel = 52; lv_dropdown_set_selected(fields_[2], 15);
        }
        lv_obj_add_flag(rows_[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(rows_[10], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(rows_[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rows_[10], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i : {2,3,4,7,8,9}) {
        bool show = i == 2 ? config_.workflow != generator::Traverse : i <= 4 ? config_.workflow == generator::Traverse : config_.workflow == generator::Pulse;
        if (show) lv_obj_remove_flag(rows_[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(rows_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (config_.workflow == generator::Single) lv_obj_remove_flag(rows_[6], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(rows_[6], LV_OBJ_FLAG_HIDDEN);
    char text[200];
    snprintf(text, sizeof(text), "%u.%02u dB attenuation; output power uncalibrated.\n%s\n%u bursts, %.3f s total.\nReboots to Ready. Tap Start there.",
        (unsigned)config_.backoff_qdb / 4, (unsigned)(config_.backoff_qdb % 4) * 25,
        config_.tone ? "CW is a narrow diagnostic tone." : "Packet: repeated 6 Mb/s OFDM.", (unsigned)generator::steps(config_), generator::total_ms(config_) / 1000.0);
    if (config_.workflow == generator::Radar) {
        const auto &p = generator::kRadar[config_.radar_profile];
        snprintf(text, sizeof(text), "%s: %.3f us pulses, %lu pulses.\nCW gate experiment. RF timing unverified; setup CW transient. Chirp/hop unavailable.\nReboots to Ready. Tap Start there.", p.name, p.width_ns / 1000.0, (unsigned long)p.pulses);
    }
    bool allowed = approved_ && lv_obj_has_state(ack_, LV_STATE_CHECKED) && generator::total_ms(config_) <= rf::kMaxSessionMs;
    if (allowed) lv_obj_remove_state(go_, LV_STATE_DISABLED); else lv_obj_add_state(go_, LV_STATE_DISABLED);
    lv_label_set_text(note_, text);
    if (!save(config_, false)) lv_label_set_text(note_, "Settings could not be saved. Check NVS before opening generator.");
}
void AirhornApp::teardown() { note_ = ack_ = go_ = nullptr; approved_ = false; for (auto &p : fields_) p = nullptr; for (auto &p : rows_) p = nullptr; }
}
