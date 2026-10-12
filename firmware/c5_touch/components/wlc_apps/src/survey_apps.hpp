// SPDX-License-Identifier: MIT
#pragma once
#include "tool_app.hpp"
#include "list_view.hpp"
#include "channel_picker.hpp"
#include "services.hpp"

namespace wlc::apps {

// Scan: every AP heard, hopping all 2.4/5 GHz channels. Clients: stations, with data-frame
// headers enabled. Passpoint: Hotspot 2.0 APs only, with ANQP queries sent while hopping.
class SurveyApp : public ToolApp {
public:
    enum class Kind { Scan, Clients, Passpoint };
    explicit SurveyApp(Kind kind);

protected:
    void build(lv_obj_t *body) override;
    void refresh() override;
    void teardown() override;

private:
    void refresh_aps();
    void refresh_clients();
    void show_ap(int index);
    void show_client(int index);
    void set_band(uint8_t mask);
    void start_radio();

    Kind kind_;
    ListView list_;
    DetailView detail_;
    ChannelPicker picker_;
    bool include_interworking_ = false;
    uint8_t band_mask_ = wlc::BAND_2G4 | wlc::BAND_5G;
    lv_obj_t *band_buttons_[3] = {};
    // Row snapshots live in PSRAM: an ApInfo is ~170 bytes and there are 30 of them.
    wlc::ApInfo *aps_ = nullptr;
    wlc::ClientInfo *clients_ = nullptr;
    uint8_t detail_mac_[6] = {};
    bool detail_is_ap_ = true;
    uint32_t ticks_ = 0;
    DetailEntry *entries_ = nullptr;   // detail page rows, PSRAM
};

}
