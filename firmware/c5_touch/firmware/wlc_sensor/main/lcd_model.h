// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "command_policy.h"

struct LcdSnapshot {
    enum Mode { IDLE, CAPTURE, RANGING, SPECTRUM, GENERATOR, TRANSMITTING } mode;
    uint32_t now, captured, forwarded, ring_drops, oversize_drops, ble_drops;
    uint32_t retries, dedup, pool_used, pool_peak, pool_bytes, channel_failures;
    uint8_t channel, width, channels[40], channel_count;
    uint8_t mac[6];
    bool connected, subscribed;
};
struct LcdFrame {
    char rows[9][27]{};
    uint8_t lengths[9]{};
    uint8_t identify = 0, palette = 0;
    uint16_t accent = 0x041f;
    void finish() { for (unsigned row=0; row<9; ++row) lengths[row]=strlen(rows[row]); }
};
struct LcdModel {
    uint32_t previous_ms = 0, previous_rx = 0, previous_tx = 0;
    uint32_t rx_rate = 0, tx_rate = 0, capture_started = 0, identify_until = 0;
    uint8_t identify_number = 0;
    bool sampled = false, capturing = false;

    static bool valid_identify(unsigned number, unsigned seconds) {
        return number >= 1 && number <= 99 && seconds >= 1 && seconds <= 15;
    }
    bool identify(unsigned number, unsigned seconds, uint32_t now) {
        if (!valid_identify(number, seconds)) return false;
        identify_number = number; identify_until = now + seconds * 1000;
        return true;
    }
    uint8_t active_identify(uint32_t now) {
        if (identify_number && (int32_t)(now - identify_until) >= 0) identify_number = 0;
        return identify_number;
    }
    void sample(const LcdSnapshot& s) {
        if (s.mode == LcdSnapshot::CAPTURE && !capturing) capture_started = s.now;
        capturing = s.mode == LcdSnapshot::CAPTURE;
        if (sampled) {
            uint32_t elapsed = s.now - previous_ms;
            if (elapsed) {
                rx_rate = (uint64_t)(uint32_t)(s.captured - previous_rx) * 1000 / elapsed;
                tx_rate = (uint64_t)(uint32_t)(s.forwarded - previous_tx) * 1000 / elapsed;
            }
        }
        previous_ms = s.now; previous_rx = s.captured; previous_tx = s.forwarded; sampled = true;
    }
    static uint32_t percentage(uint32_t bytes, uint32_t capacity) {
        return capacity ? (bytes >= capacity ? 100 : (uint64_t)bytes * 100 / capacity) : 0;
    }
    static void compact(uint32_t value, char (&output)[7]) {
        if (value < 1000) { snprintf(output, 7, "%lu", (unsigned long)value); return; }
        uint32_t unit = value < 1000000 ? 1000 : value < 1000000000 ? 1000000 : 1000000000;
        char suffix = unit == 1000 ? 'K' : unit == 1000000 ? 'M' : 'G';
        snprintf(output, 7, "%lu.%lu%c", (unsigned long)((value / unit) % 1000),
                 (unsigned long)(((value % unit) / (unit / 10)) % 10), suffix);
    }
    LcdFrame frame(const LcdSnapshot& s) {
        LcdFrame f;
        char rx[7], tx[7], loss[7], oversize[7], ble[7], retry[7], dedup[7], cherr[7];
        compact(capturing ? rx_rate : 0, rx); compact(capturing ? tx_rate : 0, tx);
        compact(s.ring_drops, loss); compact(s.oversize_drops, oversize); compact(s.ble_drops, ble);
        compact(s.retries, retry); compact(s.dedup, dedup); compact(s.channel_failures, cherr);
        snprintf(f.rows[0], 27, "WLC LCD  %02X:%02X:%02X", s.mac[3], s.mac[4], s.mac[5]);
        const char* mode = "IDLE";
        if (s.mode == LcdSnapshot::CAPTURE) { mode = "CAPTURING"; f.accent = 0x07e0; }
        if (s.mode == LcdSnapshot::RANGING) mode = "RANGING";
        if (s.mode == LcdSnapshot::SPECTRUM) { mode = "SPECTRUM"; f.accent = 0x07ff; }
        if (s.mode == LcdSnapshot::GENERATOR) { mode = "GENERATOR IDLE"; f.accent = 0xffe0; }
        if (s.mode == LcdSnapshot::TRANSMITTING) { mode = "RF ACTIVE"; f.accent = 0xf800; }
        snprintf(f.rows[1], 27, "%s", mode);
        f.identify = active_identify(s.now);
        if (f.identify) {
            f.palette = (f.identify - 1) % 6;
            snprintf(f.rows[8], 27, "IDENTIFY %u  %lus", f.identify,
                     (unsigned long)((identify_until - s.now + 999) / 1000));
            f.finish(); return f;
        }
        unsigned page = (s.now / 5000) % 3;
        if (page == 0) {
            snprintf(f.rows[2], 27, "DASHBOARD 1/3");
            snprintf(f.rows[3], 27, "BLE %s", s.connected ? "CONNECTED" : "NO LINK");
            snprintf(f.rows[4], 27, "%s", s.subscribed ? "STREAM SUBSCRIBED" : "NO STREAM SUBSCRIBER");
            uint32_t secs = capturing ? (s.now - capture_started) / 1000 : 0;
            snprintf(f.rows[5], 27, "CAPTURE %lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
            snprintf(f.rows[6], 27, "RX %s/S", rx);
            snprintf(f.rows[7], 27, "FWD %s/S", tx);
        } else if (page == 1) {
            snprintf(f.rows[2], 27, "CHANNELS 2/3");
            bool sniffer = s.mode <= LcdSnapshot::RANGING;
            if (sniffer) snprintf(f.rows[3], 27, "CURRENT %u  %s", s.channel, s.channel <= 14 ? "2.4 GHZ" : "5 GHZ");
            else snprintf(f.rows[3], 27, "SNIFFER PLAN (INACTIVE)");
            snprintf(f.rows[4], 27, "SET WIDTH %s", s.width == 1 ? "40 MHZ ABOVE" : s.width == 2 ? "40 MHZ BELOW" : "20 MHZ");
            // Every configured channel is shown, in rotating chunks of ten.
            unsigned start = s.channel_count ? ((s.now / 5000) / 3 * 10) % ((s.channel_count + 9) / 10 * 10) : 0;
            snprintf(f.rows[5], 27, "PLAN %u-%u OF %u", s.channel_count ? start + 1 : 0,
                start + 10 < s.channel_count ? start + 10 : s.channel_count, s.channel_count);
            for (unsigned i = 0; i < 10 && start + i < s.channel_count; ++i) {
                unsigned row = 6 + i / 5;
                size_t n = strlen(f.rows[row]);
                snprintf(f.rows[row] + n, 27 - n, "%s%u", i % 5 ? " " : "", s.channels[start + i]);
            }
        } else {
            snprintf(f.rows[2], 27, "CAPTURE HEALTH 3/3");
            compact(s.captured, rx); compact(s.forwarded, tx);
            snprintf(f.rows[3], 27, "RX %s FWD %s", rx, tx);
            snprintf(f.rows[4], 27, "POOL %lu%% PEAK %lu%%", (unsigned long)percentage(s.pool_used, s.pool_bytes), (unsigned long)percentage(s.pool_peak, s.pool_bytes));
            snprintf(f.rows[5], 27, "RING LOSS %s", loss);
            snprintf(f.rows[6], 27, "SIZE %s BLE %s", oversize, ble);
            snprintf(f.rows[7], 27, "RETRY %s DEDUP %s", retry, dedup);
        }
        snprintf(f.rows[8], 27, "CH ERR %s  LCD POC V1", cherr);
        f.finish(); return f;
    }
};
static inline bool lcd_usb_identify(const char* line, uint32_t& number, uint32_t& seconds) {
    if (strncmp(line, "IDENTIFY ", 9)) return false;
    const char* p = line + 9;
    return command_number(p, number) && command_number(p, seconds) && command_end(p) && LcdModel::valid_identify(number, seconds);
}
// Count complete frames accepted by the BLE stack, including completed fragments.
// The original firmware's fwd counter is pool admission, not BLE delivery.
static inline uint32_t lcd_completed_records(const uint8_t* data, size_t size) {
    uint32_t frames=0;
    while (size >= 2) {
        uint16_t header = data[0] | (uint16_t)data[1] << 8;
        size_t length = header & 0x3fff;
        if (!length || length > size-2) return frames;
        if ((header & 0xc000) == 0 || (header & 0xc000) == 0xc000) ++frames;
        data += length+2; size -= length+2;
    }
    return frames;
}
