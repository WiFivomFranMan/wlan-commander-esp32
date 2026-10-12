// SPDX-License-Identifier: GPL-3.0-or-later
// Generated from captures on 10:bd:a3:dd:89:20 (BANDWIDTH 48, 256 bins at 80 MHz): the receiver's
// noise floor by distance from DC, in ESP-SDR units (20*log10 of FFT power), 25th percentile
// across captures. SpectrumView subtracts the part above the 10-20 MHz floor, in linear power.
#pragma once
#include <stdint.h>
// 2.4 GHz: only the DC bins are corrected. These captures were dominated by real traffic (the band
// is busy here), so their percentile is not a noise floor; outside DC the band measured flat.
static const uint8_t SKIRT_24[65] = {106, 96, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70, 70};
static const uint8_t SKIRT_24_REF = 70;
// 28 captures across 5170-5835 MHz.
static const uint8_t SKIRT_5[65] = {134, 138, 132, 128, 118, 104, 106, 110, 110, 105, 96, 97, 101, 102, 99, 91, 90, 96, 98, 96, 88, 84, 91, 94, 92, 86, 78, 87, 91, 89, 84, 75, 83, 88, 88, 83, 73, 79, 84, 85, 81, 72, 76, 82, 84, 80, 72, 72, 80, 82, 79, 71, 70, 78, 80, 78, 70, 68, 76, 78, 77, 70, 65, 73, 76};
static const uint8_t SKIRT_5_REF = 65;
