// SPDX-License-Identifier: MIT
// The single owner of the Wi-Fi radio in the touch image. Apps ask for a plan; nothing else
// calls esp_wifi_* directly, so a channel change can never race a capture or a GAS request.
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace wlc::radio {

constexpr size_t kMaxChannels = 40;
constexpr size_t kMaxTargets = 24;   // same bound as the sensor firmware's Passpoint table

// Called on the radio task for every captured frame. `frame` starts at Frame Control, FCS
// already removed. The C5 is 2.4/5 GHz only, so `is_5ghz` is decided from the channel the
// radio was tuned to, never inferred from a channel number seen in the frame.
using FrameSink = void (*)(const uint8_t *frame, size_t len, int8_t rssi, uint8_t channel,
                           bool is_5ghz, uint32_t now_ms);

// Writes a GAS Initial Request action body into `out`; returns its length (0 = none).
using QueryBuilder = size_t (*)(uint8_t *out, size_t cap, uint8_t dialog_token, int query_set);

struct Plan {
    uint8_t channels[kMaxChannels];
    uint8_t channel_count;
    uint16_t dwell_ms;          // per channel
    bool data_frames;           // Clients needs data headers; Scan does not
};

struct Target {
    uint8_t bssid[6];
    uint8_t channel;
};

struct Stats {
    uint32_t captured;
    uint32_t ring_drops;        // the ring was full: frames lost to a slow consumer
    uint32_t oversize;
    uint32_t hops;
    uint32_t hop_failures;
    uint32_t queries_sent;
    uint32_t query_failures;
    uint8_t channel;
    bool running;
};

esp_err_t init(FrameSink sink, QueryBuilder builder);
esp_err_t start(const Plan &plan);
esp_err_t stop();
// Replaces the ANQP target list. Each target is queried while the hop dwells on its channel,
// at most once per `interval_ms`, alternating the two query sets.
esp_err_t set_targets(const Target *targets, size_t count, uint32_t interval_ms);
Stats stats();
Plan current_plan();
void own_mac(uint8_t out[6]);

}
