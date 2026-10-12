// SPDX-License-Identifier: MIT
// Which channels the survey tools sweep, chosen on screen and kept in NVS. The C5 is 2.4/5 GHz only.
#pragma once
#include <cstdint>
#include "wlc_radio.hpp"

namespace wlc::apps {

enum ChannelGroup : uint8_t {
    GROUP_24 = 1u << 0,       // 2.4 GHz
    GROUP_UNII1 = 1u << 1,    // 36-48
    GROUP_UNII2 = 1u << 2,    // 52-64, DFS
    GROUP_UNII2E = 1u << 3,   // 100-144, DFS
    GROUP_UNII3 = 1u << 4,    // 149-165
};

struct ChannelSettings {
    uint8_t groups = GROUP_24 | GROUP_UNII1 | GROUP_UNII2 | GROUP_UNII2E | GROUP_UNII3;
    bool all_24 = false;      // false: 1, 6, 11 only; true: 1-11
    uint16_t dwell_ms = 250;
};

struct GroupInfo { ChannelGroup bit; const char *name; const char *channels; };
extern const GroupInfo kGroups[5];

ChannelSettings &channel_settings();            // loaded from NVS on first use
void save_channel_settings();
// Fills a radio plan from the settings; `min_dwell_ms` lets Passpoint keep room for replies.
void build_plan(radio::Plan &plan, bool data_frames, uint16_t min_dwell_ms);

}
