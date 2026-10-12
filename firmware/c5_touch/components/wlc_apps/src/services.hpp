// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "wlc_survey/survey.h"
#include "wlc_survey/anqp.h"

namespace wlc::apps {

// The one survey every app reads. Fed by the radio task; read under its own lock.
wlc::Survey &survey();

// Channel plans the apps share. The C5 is 2.4/5 GHz only.
extern const uint8_t kAllChannels[];
extern const uint8_t kAllChannelCount;

// Points the radio's ANQP pokes at the Passpoint APs heard so far (at most 24). Returns how many.
size_t refresh_passpoint_targets(bool include_interworking = false);

// Signal colour: red is strongest. This matches WLAN Commander's heatmaps; do not invert it.
uint32_t rssi_color(int8_t rssi);

}
