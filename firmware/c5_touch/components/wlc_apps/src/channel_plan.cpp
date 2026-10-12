// SPDX-License-Identifier: MIT
#include "channel_plan.hpp"

#include "nvs.h"

namespace wlc::apps {

const GroupInfo kGroups[5] = {
    {GROUP_24, "2.4 GHz", "1, 6, 11"},
    {GROUP_UNII1, "5 GHz UNII-1", "36-48"},
    {GROUP_UNII2, "5 GHz UNII-2 (DFS)", "52-64"},
    {GROUP_UNII2E, "5 GHz UNII-2e (DFS)", "100-144"},
    {GROUP_UNII3, "5 GHz UNII-3", "149-165"},
};

namespace {

ChannelSettings g_settings;
bool g_loaded;

void add_range(radio::Plan &p, int from, int to, int step)
{
    for (int ch = from; ch <= to && p.channel_count < radio::kMaxChannels; ch += step)
        p.channels[p.channel_count++] = (uint8_t)ch;
}

}  // namespace

ChannelSettings &channel_settings()
{
    if (!g_loaded) {
        g_loaded = true;
        nvs_handle_t h;
        if (nvs_open("wlc", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            uint16_t d;
            if (nvs_get_u8(h, "ch_groups", &v) == ESP_OK && (v & 0x1f)) g_settings.groups = v & 0x1f;
            if (nvs_get_u8(h, "ch_all24", &v) == ESP_OK) g_settings.all_24 = v != 0;
            if (nvs_get_u16(h, "ch_dwell", &d) == ESP_OK && d >= 50 && d <= 2000) g_settings.dwell_ms = d;
            nvs_close(h);
        }
    }
    return g_settings;
}

void save_channel_settings()
{
    nvs_handle_t h;
    if (nvs_open("wlc", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ch_groups", g_settings.groups);
    nvs_set_u8(h, "ch_all24", g_settings.all_24 ? 1 : 0);
    nvs_set_u16(h, "ch_dwell", g_settings.dwell_ms);
    nvs_commit(h);
    nvs_close(h);
}

void build_plan(radio::Plan &plan, bool data_frames, uint16_t min_dwell_ms)
{
    const ChannelSettings &s = channel_settings();
    plan = radio::Plan{};
    if (s.groups & GROUP_24) {
        if (s.all_24) add_range(plan, 1, 11, 1);
        else add_range(plan, 1, 11, 5);
    }
    if (s.groups & GROUP_UNII1) add_range(plan, 36, 48, 4);
    if (s.groups & GROUP_UNII2) add_range(plan, 52, 64, 4);
    if (s.groups & GROUP_UNII2E) add_range(plan, 100, 144, 4);
    if (s.groups & GROUP_UNII3) add_range(plan, 149, 165, 4);
    if (!plan.channel_count) add_range(plan, 1, 11, 5);
    plan.dwell_ms = s.dwell_ms > min_dwell_ms ? s.dwell_ms : min_dwell_ms;
    plan.data_frames = data_frames;
}

}
