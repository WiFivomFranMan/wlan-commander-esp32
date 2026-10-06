// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental combined WLAN Commander / ESP-SDR firmware.
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "esp32-hal-alloc-ble-mem.h"
#include <atomic>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "soc/soc.h"
#include "command_policy.h"
#include "generator_policy.h"
#include "esp_phy_cert_test.h"
extern "C" {
#include "burst_serial.h"
void wlc_sdr_reset(void);
void wlc_sdr_dispatch(char*);
}

static void boot_trace(const char* text) {
#ifdef WLC_SDR_BOOT_TRACE
    burst_serial_send(text, strlen(text));
#else
    (void)text;
#endif
}

#ifdef WLC_SDR_BOOT_TRACE
extern "C" void initVariant(void) {
    burst_serial_init();
    boot_trace("BOOT Arduino initialized\n");
}
#endif

static std::atomic<bool> radio_gate{false};
struct QuietSerial {
    void begin(unsigned) {}
    void println(const char*) {}
};
static QuietSerial quiet_serial;
#include "sniffer.inc"

static bool spectrum_mode;
static bool generator_mode, generator_active, generator_tone;
static uint32_t generator_channel_value, generator_backoff;
static int64_t generator_deadline;
static std::atomic<uint32_t> generator_guard_ms{0};
static std::atomic<bool> packet_worker_running{false};
// Consume this request before RF initialization. A later failure boots idle.
RTC_NOINIT_ATTR static uint32_t generator_boot_magic;
RTC_NOINIT_ATTR static uint32_t generator_boot_inverse;
static constexpr uint32_t GENERATOR_BOOT_MAGIC = 0x574c4347;
[[noreturn]] static void generator_reset() {
    // Same full RTC-watchdog reset used by esptool's ESP32C5ROM. The normal
    // software restart can remain in ROM after this chip's raw PHY mode.
    REG_WRITE(0x600b1c18, 0x50d83aa1);
    REG_WRITE(0x600b1c04, 2000);
    REG_WRITE(0x600b1c00, 0xd0000102);
    REG_WRITE(0x600b1c18, 0);
    for (;;) vTaskDelay(1);
}
static void guard_arm(uint32_t ms) {
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + ms + 100;
    generator_guard_ms.store(deadline ? deadline : 1);
}
static void guard_check(void*) {
    uint32_t deadline = generator_guard_ms.load();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // Separate esp_timer task: a stuck private PHY call cannot keep RF on.
    // Reboot is the fail-off fallback; the normal loop stops and restores BLE.
    if (deadline && (int32_t)(now - deadline) >= 0 && deadline == generator_guard_ms.load()) generator_reset();
}

static void generator_stop() {
    if (!generator_mode) return;
    guard_arm(5000);
    if (generator_tone) esp_phy_wifi_tx_tone(0, generator_channel_value, generator_backoff);
    esp_phy_test_start_stop(0);
    // The SDK's packet_num=0 implementation loops in WifiTxStart until its
    // stop callback becomes zero. Keep that loop off the USB command task.
    while (packet_worker_running.load()) delay(1);
    esp_phy_tx_contin_en(false);
    generator_active = false;
    generator_deadline = 0;
    generator_guard_ms.store(0);
}
static void packet_worker(void*) {
    esp_phy_cbw40m_en(false);
    esp_phy_tx_contin_en(true);
    esp_phy_wifi_tx(generator_channel_value, PHY_RATE_6M,
                    (int8_t)generator_backoff, 1000, 0, 0);
    packet_worker_running.store(false);
    vTaskDelete(nullptr);
}
static int64_t last_command_us;
// Excluding the bank from the heap lets the exclusive modes share it explicitly.
// Packets occupy its first 64 KiB; SDR IQ uses the second 64 KiB.
static_assert(POOL_BYTES <= 65536, "Packet pool exceeds shared SRAM half-bank");

static void reply(const char* text) { burst_serial_send(text, strlen(text)); }

static bool restore_sniffer() {
    // A complete Wi-Fi teardown discards all private RF-test state before
    // Arduino starts the normal receiver again. Capture remains explicitly idle.
    esp_wifi_stop();
    if (esp_wifi_deinit() != ESP_OK) return false;
    wlc_sdr_reset();
    wantRun = isRun = false;
    subscribed = false;
    connH = 0xffff;
    discAt = 0;
    if (!WiFi.mode(WIFI_MODE_STA)) return false;
    WiFi.setBandMode(WIFI_BAND_MODE_AUTO);
    WiFi.disconnect();
    if (esp_wifi_set_promiscuous(false) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(&promisc_cb) != ESP_OK ||
        esp_wifi_set_channel(cur_ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    if (!ble_setup()) return false;
    generator_guard_ms.store(0);
    spectrum_mode = generator_mode = false;
    radio_gate.store(false);
    return true;
}

static bool enter_spectrum() {
    radio_gate.store(true);
    portENTER_CRITICAL(&ftmMux);
    bool busy = isRun || wantRun || ftmActive || ftmHasQueued;
    portEXIT_CRITICAL(&ftmMux);
    if (busy) { radio_gate.store(false); return false; }
    // Deinitialization frees dynamic BLE allocations; it does not permanently
    // release controller SRAM, so the same firmware can initialize BLE again.
    if (!NimBLEDevice::deinit(true)) return false;
    // A callback may have passed the gate just before we closed it. With the
    // host now stopped, recheck its mailbox before changing the Wi-Fi driver.
    portENTER_CRITICAL(&ftmMux);
    busy = isRun || wantRun || ftmActive || ftmHasQueued;
    portEXIT_CRITICAL(&ftmMux);
    frameChar = statChar = ftmChar = nullptr;
    subscribed = false;
    connH = 0xffff;
    if (busy) {
        if (!ble_setup()) esp_restart();
        radio_gate.store(false);
        return false;
    }
    if (!WiFi.mode(WIFI_MODE_NULL)) return false;
    pool = nullptr; rhead = rtail = 0;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK ||
        esp_wifi_start() != ESP_OK ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_promiscuous(true) != ESP_OK ||
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
    wlc_sdr_reset();
    spectrum_mode = true;
    last_command_us = esp_timer_get_time();
    return true;
}

static bool enter_generator() {
    radio_gate.store(true);
    portENTER_CRITICAL(&ftmMux);
    bool busy = isRun || wantRun || ftmActive || ftmHasQueued;
    portEXIT_CRITICAL(&ftmMux);
    if (busy) { radio_gate.store(false); return false; }
    // The private PHY does not start packet TX reliably after normal Wi-Fi
    // teardown on this C5. A fresh boot works with the same scratch layout.
    generator_boot_magic = GENERATOR_BOOT_MAGIC;
    generator_boot_inverse = ~GENERATOR_BOOT_MAGIC;
    reply("OK REBOOT\n");
    delay(20);
    esp_restart();
}

void setup() {
    // Neither Arduino Serial nor the SDK console may consume this USB endpoint.
    esp_log_level_set("*", ESP_LOG_NONE);
    burst_serial_init();
    boot_trace("BOOT setup\n");
    WiFi.useStaticBuffers(true); // honor the bounded sdkconfig buffer counts
    const bool boot_generator = generator_boot_magic == GENERATOR_BOOT_MAGIC &&
                                generator_boot_inverse == ~GENERATOR_BOOT_MAGIC;
    generator_boot_magic = generator_boot_inverse = 0;
    esp_timer_handle_t guard_timer;
    esp_timer_create_args_t guard_args = {};
    guard_args.callback = guard_check;
    guard_args.name = "rf-fail-off";
    ESP_ERROR_CHECK(esp_timer_create(&guard_args, &guard_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(guard_timer, 50000));
    if (boot_generator) {
        esp_read_mac(myMac, ESP_MAC_WIFI_STA);
        radio_gate.store(true);
        memset(reinterpret_cast<void*>(0x40820000), 0, 65536);
        guard_arm(5000);
        esp_wifi_power_domain_on();
        esp_phy_rftest_config(1);
        esp_phy_rftest_init();
        generator_mode = true;
        generator_stop();
    } else {
        sniffer_setup();
    }
    boot_trace("BOOT radio initialized\n");
    burst_serial_init();
    last_command_us = esp_timer_get_time();
}

void loop() {
    char line[128];
    int status = burst_serial_poll_line(line, sizeof(line));
    if (status < 0) reply("ERR command_length\n");
    if (status > 0) {
        if (!generator_mode || generator_heartbeat(line))
            last_command_us = esp_timer_get_time();
        if (!strcmp(line, "MODE?")) {
            reply(generator_mode ? "MODE GENERATOR\n" : spectrum_mode ? "MODE SPECTRUM\n" : "MODE SNIFFER\n");
        } else if (!strcmp(line, "TXINFO?")) {
            char text[384];
            snprintf(text, sizeof(text), "TXINFO {\"mode\":\"%s\",\"active\":%s,\"waveform\":\"%s\",\"channel\":%u,\"backoff_qdb\":%u,\"remaining_ms\":%lld,\"max_duration_ms\":30000,\"lease_ms\":5000,\"channels\":[1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,149,153,157,161,165],\"power_calibrated\":false}\n",
                generator_mode ? "generator" : spectrum_mode ? "spectrum" : "sniffer",
                generator_active ? "true" : "false", generator_tone ? "cw" : "packet",
                (unsigned)generator_channel_value, (unsigned)generator_backoff,
                (long long)(generator_active && generator_deadline > esp_timer_get_time() ?
                    (generator_deadline - esp_timer_get_time()) / 1000 : 0));
            reply(text);
        } else if (!strcmp(line, "PING")) {
            reply("OK\n");
        } else if (!strcmp(line, "TXSTOP")) {
            generator_stop(); reply("OK\n");
        } else if (!strncmp(line, "TX ", 3)) {
            GeneratorRequest request;
            if (!generator_mode) reply("ERR generator_mode_required\n");
            else if (!generator_request(line, request)) reply("ERR generator_args\n");
            else if (generator_active) reply("ERR busy\n");
            else {
                last_command_us = esp_timer_get_time();
                generator_tone = request.tone;
                generator_channel_value = request.channel;
                generator_backoff = request.backoff_qdb;
                generator_deadline = esp_timer_get_time() + (int64_t)request.duration_ms * 1000;
                guard_arm(request.duration_ms < 5000 ? request.duration_ms : 5000);
                bool started = true;
                esp_phy_cbw40m_en(false);
                if (request.tone) esp_phy_wifi_tx_tone(1, request.channel, request.backoff_qdb);
                else {
                    esp_phy_test_start_stop(3);
                    packet_worker_running.store(true);
                    if (xTaskCreate(packet_worker, "rf-packets", 10240, nullptr, 1, nullptr) != pdPASS) {
                        packet_worker_running.store(false);
                        generator_stop();
                        started = false;
                    }
                }
                generator_active = started;
                reply(started ? "OK\n" : "ERR generator_allocation\n");
            }
        } else if (!strcmp(line, "MODE GENERATOR")) {
            if (spectrum_mode) reply("ERR busy\n");
            else if (generator_mode || enter_generator()) reply("OK\n");
            else if (radio_gate.load()) { reply("ERR mode_transition\n"); esp_restart(); }
            else reply("ERR busy\n");
        } else if (!strcmp(line, "MODE SPECTRUM")) {
            if (generator_mode) reply("ERR busy\n");
            else if (spectrum_mode || enter_spectrum()) reply("OK\n");
            else if (radio_gate.load()) {
                reply("ERR mode_transition\n");
                esp_restart(); // restore the boot-idle contract after partial teardown
            } else reply("ERR busy\n");
        } else if (!strcmp(line, "MODE SNIFFER") || !strcmp(line, "RELEASE")) {
            if (generator_mode) {
                generator_stop();
                reply("OK REBOOT\n");
                delay(20);
                generator_reset();
            }
            if (!spectrum_mode || restore_sniffer()) reply("OK\n");
            else { reply("ERR mode_restore\n"); esp_restart(); }
        } else if (!strcmp(line, "WLCINFO?")) {
            char text[384];
            snprintf(text, sizeof(text),
                "WLCINFO {\"firmware\":\"airhorn-dev\",\"generator_lease\":2,\"mode\":\"%s\",\"run\":%s,\"ftm\":%s,\"heap\":%u,\"largest_internal\":%u,\"ble_initialized\":%s,\"shared_packet_pool\":true,\"fft_max\":512,\"bands_mhz\":[[2400,2483],[5150,5895]],\"board\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
                generator_mode ? "generator" : spectrum_mode ? "spectrum" : "sniffer", isRun ? "true" : "false",
                ftmActive ? "true" : "false", (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                NimBLEDevice::isInitialized() ? "true" : "false",
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
            reply(text);
        } else if (!strcmp(line, "CAPS")) {
            reply("CAPS SPEC SPECN SPECCAPS SPECSTAT DCT RXLIMITS GAIN HWAGC WLCMODE WLCGENERATOR\n");
        } else if (!strcmp(line, "RANGE?")) {
            // The protocol's RANGE is an envelope; the policy rejects the gap
            // between the supported WLAN bands.
            reply("RANGE 2400 5895 1\n");
        } else if (generator_mode) {
            reply("ERR generator_command\n");
        } else if (!spectrum_mode && !idle_query(line)) {
            reply("ERR spectrum_mode_required\n");
        } else if (spectrum_mode && !spectrum_request(line)) {
            reply("ERR spectrum_args\n");
        } else {
            wlc_sdr_dispatch(line);
            last_command_us = esp_timer_get_time();
        }
    }
    if (status > 0 && generator_mode && generator_active) {
        uint32_t remaining = generator_guard_remaining(esp_timer_get_time(), last_command_us, generator_deadline);
        if (remaining) guard_arm(remaining);
    }
    if (generator_mode && generator_active && esp_timer_get_time() >= generator_deadline)
        generator_stop();
    if ((spectrum_mode || generator_mode) && esp_timer_get_time() - last_command_us > 5000000) {
        if (generator_mode) {
            generator_stop();
            generator_reset();
        }
        if (!restore_sniffer()) {
            // Recovery cannot pretend BLE has returned. Reboot to the idle boot
            // contract when restoring a private PHY state fails.
            esp_restart();
        }
    }
    if (!spectrum_mode && !generator_mode && !radio_gate.load()) {
        if (wantRun && !pool) {
            // Bind only in normal mode, before sniffer_loop enables reception.
            // acquire_iq restores CPU ownership after each SDR snapshot; mode
            // restoration also completely restarts the Wi-Fi driver.
            pool = reinterpret_cast<uint8_t*>(0x40820000);
        }
        sniffer_loop();
    }
    delay(1);
}
