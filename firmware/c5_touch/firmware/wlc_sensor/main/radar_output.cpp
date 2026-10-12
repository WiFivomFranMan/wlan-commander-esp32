// SPDX-License-Identifier: GPL-3.0-or-later
#include "radar_output.h"
#include "wlc_radar.hpp"
#include "esp_phy_cert_test.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "soc/soc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <stdio.h>

namespace {
// EXPERIMENTAL, pinned C5 PHY only. prepare_sources.py hash-checks libphy.a
// and librftest.a. Disassembly shows wifiscwout selects the first tone path;
// phy_start_tx_tone_step writes bit 18 at 0x600a041c; phy_stop_tx_tone clears it.
// This is software evidence,
// not qualification of radiated pulse timing, RF transients or AP detection.
constexpr uint32_t TONE_CFG = 0x600a041c, TONE_ENABLE = 1u << 18;
std::atomic<bool> running{false}, cancel{false}, failed{false};
std::atomic<uint32_t> emitted{0}, max_width_ns{0}, max_late_ns{0}, prepare_us{0};
uint32_t channel, backoff, selected;
portMUX_TYPE pulse_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t IRAM_ATTR pulse(uint32_t on, uint32_t off, uint32_t width_cycles) {
    portENTER_CRITICAL(&pulse_mux);
    uint32_t begin = esp_cpu_get_cycle_count();
    REG_WRITE(TONE_CFG, on);
    while ((uint32_t)(esp_cpu_get_cycle_count() - begin) < width_cycles) {}
    REG_WRITE(TONE_CFG, off);
    uint32_t elapsed = esp_cpu_get_cycle_count() - begin;
    portEXIT_CRITICAL(&pulse_mux);
    return elapsed;
}
void worker(void *) {
    const auto &p = wlc::generator::kRadar[selected];
    const uint32_t mhz = esp_clk_cpu_freq() / 1000000;
    uint32_t prep = (uint32_t)esp_timer_get_time();
    // The public API tunes/calibrates and briefly enables CW before we gate it
    // off. This setup transient is deliberately disclosed in the UI/docs.
    esp_phy_cbw40m_en(false);
    esp_phy_wifi_tx_tone(1, channel, backoff);
    uint32_t on = REG_READ(TONE_CFG), off = on & ~TONE_ENABLE;
    REG_WRITE(TONE_CFG, off);
    prepare_us.store((uint32_t)esp_timer_get_time() - prep);
    if (!(on & TONE_ENABLE) || !mhz) failed.store(true);
    const uint32_t width = (uint64_t)p.width_ns * mhz / 1000;
    uint32_t next = esp_cpu_get_cycle_count() + mhz * 100; // blank after preparation
    for (uint32_t i = 0; i < p.pulses && !failed.load(); ++i) {
        while ((int32_t)(esp_cpu_get_cycle_count() - next) < 0 && !cancel.load()) {}
        if (cancel.load()) break;
        uint32_t late = esp_cpu_get_cycle_count() - next;
        uint32_t period = (uint64_t)p.pri_ns[i % p.pri_count] * mhz / 1000;
        if (late > period / 4) { failed.store(true); break; } // do not compress missed intervals
        uint32_t late_ns = (uint64_t)late * 1000 / mhz;
        if (late_ns > max_late_ns.load()) max_late_ns.store(late_ns);
        uint32_t ns = (uint64_t)pulse(on, off, width) * 1000 / mhz;
        if (ns > max_width_ns.load()) max_width_ns.store(ns);
        emitted.store(i + 1);
        next += period;
    }
    REG_WRITE(TONE_CFG, off);
    esp_phy_wifi_tx_tone(0, channel, backoff);
    running.store(false);
    vTaskDelete(nullptr);
}
}
bool wlc_radar_start(uint32_t ch, uint32_t atten, uint32_t profile) {
    if (running.load() || !wlc::generator::dfs_channel(ch) || atten > 80 || profile >= wlc::generator::kRadarCount) return false;
    channel = ch; backoff = atten; selected = profile;
    cancel.store(false); failed.store(false); emitted.store(0); max_width_ns.store(0); max_late_ns.store(0); prepare_us.store(0);
    running.store(true);
    // Each complete event is <100 ms. Priority 2 prevents SPI/touch rendering
    // from stretching the PRI; the independent esp_timer fail-off still runs.
    // Stop is serviced after this bounded event, never an unbounded train.
    if (xTaskCreate(worker, "dfs-event", 4096, nullptr, 2, nullptr) != pdPASS) {
        running.store(false); failed.store(true); return false;
    }
    return true;
}
bool wlc_radar_running() { return running.load(); }
bool wlc_radar_failed() { return failed.load(); }
void wlc_radar_cancel() { cancel.store(true); while (running.load()) vTaskDelay(1); }
void wlc_radar_info(char *out, size_t capacity) {
    snprintf(out, capacity, "{\"experimental\":true,\"rf_timing_verified\":false,\"setup_transient\":true,\"profile\":%lu,\"pulses\":%lu,\"max_software_width_ns\":%lu,\"max_software_late_ns\":%lu,\"prepare_us\":%lu,\"running\":%s,\"failed\":%s}",
        (unsigned long)selected, (unsigned long)emitted.load(), (unsigned long)max_width_ns.load(),
        (unsigned long)max_late_ns.load(), (unsigned long)prepare_us.load(), running.load() ? "true" : "false", failed.load() ? "true" : "false");
}
