// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "wlc_radar.hpp"

namespace wlc::generator {
constexpr uint32_t kVersion = 2;
constexpr const char *kImageVersion = "touch-airhorn-v3";
constexpr uint32_t kChannels[] = {1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
enum Workflow : uint32_t { Single, Traverse, Pulse, Radar };
// Fixed-width, padding-free NVS representation shared by the two images.
struct Config {
    uint32_t version = kVersion, workflow = Single, tone = 0, band5 = 0;
    uint32_t channel = 1, backoff_qdb = 80, single_ms = 5000, on_ms = 1000, off_ms = 1000;
    uint32_t count = 3, dwell_ms = 5000, radar_profile = 0;
};
static_assert(sizeof(Config) == 48, "NVS config layout changed");
inline bool channel_valid(uint32_t channel) {
    for (auto c : kChannels) if (c == channel) return true;
    return false;
}
inline bool valid(const Config &c) {
    return c.version == kVersion && c.workflow <= Radar && c.tone <= 1 && c.band5 <= 1 &&
        channel_valid(c.channel) && c.backoff_qdb <= 80 && c.single_ms >= 1 && c.single_ms <= 30000 && c.on_ms >= 1 && c.on_ms <= 30000 &&
        c.off_ms >= 1 && c.off_ms <= 30000 && c.count >= 1 && c.count <= 100 &&
        (c.dwell_ms == 5000 || c.dwell_ms == 30000) && c.radar_profile < kRadarCount &&
        (c.workflow != Radar || (dfs_channel(c.channel) && c.tone == 1));
}
inline uint32_t steps(const Config &c) { return c.workflow == Traverse ? (c.band5 ? 25 : 11) : c.workflow == Pulse ? c.count : 1; }
inline uint32_t duration(const Config &c) { return c.workflow == Radar ? (radar_duration_us(kRadar[c.radar_profile]) + 999) / 1000 : c.workflow == Traverse ? c.dwell_ms : c.workflow == Single ? c.single_ms : c.on_ms; }
inline uint32_t channel_at(const Config &c, uint32_t index) { return c.workflow == Traverse ? kChannels[(c.band5 ? 11 : 0) + index] : c.channel; }
inline uint32_t total_ms(const Config &c) { return steps(c) * duration(c) + (c.workflow == Pulse ? (steps(c) - 1) * c.off_ms : 0); }
enum class Phase : uint8_t { Ready, Active, Gap, Stopped, Complete, Error };
inline const char *phase_name(Phase p) {
    switch (p) {
    case Phase::Ready: return "READY"; case Phase::Active: return "RF ACTIVE";
    case Phase::Gap: return "GAP"; case Phase::Stopped: return "STOPPED";
    case Phase::Complete: return "COMPLETE"; default: return "ERROR";
    }
}
struct Run {
    Config config;
    Phase phase = Phase::Ready;
    uint32_t index = 0, deadline = 0;
    bool running() const { return phase == Phase::Active || phase == Phase::Gap; }
    bool due(uint32_t now) const { return running() && (int32_t)(now - deadline) >= 0; }
    uint32_t remaining(uint32_t now) const { return running() && !due(now) ? deadline - now : 0; }
    bool start(uint32_t now) {
        if (running() || phase == Phase::Error || !valid(config)) return false;
        index = 0; phase = Phase::Active; deadline = now + duration(config); return true;
    }
    // Caller stops the current PHY burst BEFORE advancing and starts a new
    // burst only after this returns Active. Late service never skips bursts.
    void advance(uint32_t now) {
        if (!due(now)) return;
        if (phase == Phase::Active) {
            if (index + 1 == steps(config)) { phase = Phase::Complete; return; }
            if (config.workflow == Pulse) { phase = Phase::Gap; deadline = now + config.off_ms; return; }
        }
        ++index; phase = Phase::Active; deadline = now + duration(config);
    }
    void stop() { if (phase != Phase::Error) phase = Phase::Stopped; deadline = 0; }
    void fail() { phase = Phase::Error; deadline = 0; }
};
// Native touch controllers report portrait coordinates. MADCTL=MV|MX maps
// portrait (x,y) to landscape (y,239-x); virtual taps already use landscape.
inline bool landscape(uint16_t px, uint16_t py, unsigned width, int &x, int &y) {
    if (px >= 240 || py >= width) return false;
    x = py; y = 239 - px; return true;
}
enum class Action { None, Start, Stop, Return };
inline Action hit(int x, int y, unsigned width) {
    if (x < 4 || x >= (int)width - 4 || y < 184 || y >= 236) return Action::None;
    unsigned third = (unsigned)x * 3 / width;
    return third == 0 ? Action::Start : third == 1 ? Action::Stop : Action::Return;
}
// One action per fresh press, including after a reboot with a finger held down.
struct Press {
    bool released = false, down = false;
    Action update(bool pressed, int x, int y, unsigned width, bool fresh = true) {
        if (!pressed) { released = true; down = false; return Action::None; }
        if (!released || down) return Action::None;
        down = true; if (!fresh) return Action::None; return hit(x, y, width);
    }
};
}
