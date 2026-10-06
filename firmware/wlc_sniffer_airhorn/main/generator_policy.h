// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "command_policy.h"

struct GeneratorRequest {
    bool tone;
    uint32_t channel, backoff_qdb, duration_ms;
};

// Observers must never extend an owner's RF lease. TX renews only after its
// arguments and ownership have been accepted; PING is the explicit heartbeat.
static bool generator_heartbeat(const char* line) {
    return !strcmp(line, "PING");
}

static uint32_t generator_guard_remaining(int64_t now, int64_t last, int64_t end) {
    int64_t lease = last + 5000000 - now;
    int64_t burst = end - now;
    int64_t remaining = lease < burst ? lease : burst;
    return remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
}

static bool generator_channel(uint32_t channel) {
    return (channel >= 1 && channel <= 11) || channel == 36 || channel == 40 ||
        channel == 44 || channel == 48 || channel == 149 || channel == 153 ||
        channel == 157 || channel == 161 || channel == 165;
}

static bool generator_request(const char* line, GeneratorRequest& request) {
    const char* p;
    bool tone;
    if (!strncmp(line, "TX PACKET ", 10)) { p = line + 10; tone = false; }
    else if (!strncmp(line, "TX CW ", 6)) { p = line + 6; tone = true; }
    else return false;
    uint32_t channel, backoff, duration;
    if (!command_number(p, channel) || !command_number(p, backoff) ||
        !command_number(p, duration) || !command_end(p) ||
        !generator_channel(channel) || backoff > 80 || !duration || duration > 30000)
        return false;
    request = {tone, channel, backoff, duration};
    return true;
}
