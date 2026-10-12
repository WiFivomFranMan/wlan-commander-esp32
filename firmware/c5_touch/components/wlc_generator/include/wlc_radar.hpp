// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
namespace wlc::generator {
// Fixed event examples within the published ranges, NOT randomized compliance
// trial suites. FCC: Keysight N7607 FCC 13-22A1 profile documentation, table 1.
// ETSI: EN 301 893 V2.2.1 Annex D, tables D.3/D.4. Chirp/hop are not synthesized.
struct RadarPattern {
    const char *name;
    uint32_t width_ns, pri_ns[3];
    uint32_t pulses, pri_count;
};
constexpr RadarPattern kRadar[] = {
    {"FCC 0", 1000, {1428000}, 18, 1},
    {"FCC 1 example", 1000, {518000}, 102, 1},
    {"FCC 2 example", 3000, {190000}, 26, 1},
    {"FCC 3 example", 8000, {350000}, 17, 1},
    {"FCC 4 example", 15000, {350000}, 14, 1},
    {"ETSI reference", 1000, {1428571}, 18, 1},
    {"ETSI 1 example", 1000, {1000000}, 10, 1},
    {"ETSI 2 example", 5000, {625000}, 15, 1},
    {"ETSI 3 example", 5000, {333333}, 25, 1},
    {"ETSI 5 stagger", 1000, {3030303,2857143,2702703}, 30, 3}, // 330/350/370 PPS
    {"ETSI 6 stagger", 1000, {1666667,1250000,1000000}, 45, 3}, // 600/800/1000 PPS
};
constexpr unsigned kRadarCount = sizeof(kRadar) / sizeof(kRadar[0]);
inline bool dfs_channel(uint32_t c) { return (c >= 52 && c <= 64 && c % 4 == 0) || (c >= 100 && c <= 144 && c % 4 == 0); }
inline uint32_t radar_duration_us(const RadarPattern &p) {
    uint64_t ns = p.width_ns;
    for (uint32_t i = 0; i + 1 < p.pulses; ++i) ns += p.pri_ns[i % p.pri_count];
    return (uint32_t)((ns + 999) / 1000);
}
}
