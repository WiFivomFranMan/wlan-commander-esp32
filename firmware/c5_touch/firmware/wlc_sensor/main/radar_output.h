// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
bool wlc_radar_start(uint32_t channel, uint32_t backoff, uint32_t profile);
bool wlc_radar_running();
bool wlc_radar_failed();
void wlc_radar_cancel();
void wlc_radar_info(char *output, size_t capacity);
