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
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "soc/soc.h"
#include "command_policy.h"
#include "generator_policy.h"
#include "wlc_generator.hpp"
#include "rf_approval.hpp"
#include "radar_output.h"
#include "lcd_display.h"
// The touch board has no addressable LED (GPIO8 is microSD MISO); the sketch's LED code compiles out.
#undef RGB_BUILTIN
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
static uint32_t lcd_ble_forwarded;
static volatile uint32_t lcd_ring_drops;
class LcdInfoCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        char text[256]; wlc_lcd_info(text, sizeof(text)); c->setValue(text);
    }
};
#include "sniffer.inc"

static bool spectrum_mode;
static bool generator_mode, generator_active, generator_tone;
static bool radar_burst;
static bool rf_approved;
static uint32_t rf_limit_ms = wlc::rf::kMaxSessionMs;
static wlc::rf::Session rf_session;
static uint32_t generator_channel_value, generator_backoff;
static int64_t generator_deadline;
static std::atomic<uint32_t> generator_guard_ms{0};
static std::atomic<bool> packet_worker_running{false};
// Consume this request before RF initialization. A later failure boots idle.
RTC_NOINIT_ATTR static uint32_t generator_boot_magic;
RTC_NOINIT_ATTR static uint32_t generator_boot_inverse;
RTC_NOINIT_ATTR static uint32_t generator_boot_budget;
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
    if (radar_burst) { wlc_radar_cancel(); radar_burst = false; }
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
static bool local_generator;
static wlc::generator::Run local_run;

// Same finite PHY operation for host and local ownership. UI work is never
// performed between arming the deadline and starting the private PHY call.
static bool generator_start(const GeneratorRequest &request) {
    if (!rf_approved || !rf_session.allows(millis(), request.duration_ms)) return false;
    generator_tone = request.tone;
    generator_channel_value = request.channel;
    generator_backoff = request.backoff_qdb;
    generator_deadline = esp_timer_get_time() + (int64_t)request.duration_ms * 1000;
    guard_arm(request.duration_ms < 5000 ? request.duration_ms : 5000);
    esp_phy_cbw40m_en(false);
    bool started = true;
    if (request.tone) esp_phy_wifi_tx_tone(1, request.channel, request.backoff_qdb);
    else {
        esp_phy_test_start_stop(3);
        packet_worker_running.store(true);
        if (xTaskCreate(packet_worker, "rf-packets", 10240, nullptr, 1, nullptr) != pdPASS) {
            packet_worker_running.store(false); generator_stop(); started = false;
        }
    }
    generator_active = started;
    return started;
}

// NVS is shared with the launcher. A request is valid only after its removal
// has been committed: no subsequent reset can replay it or start RF.
static bool take_generator_request(wlc::generator::Config &config) {
    nvs_handle_t h;
    if (nvs_open("wlc", NVS_READWRITE, &h) != ESP_OK) return false;
    wlc::generator::Config candidate;
    uint8_t request = 0, acknowledged = 0; size_t size = sizeof(candidate);
    bool found = nvs_get_u8(h, "boot_generator", &request) == ESP_OK;
    bool valid = found && rf_approved && nvs_get_u8(h, "rf_shield_ack", &acknowledged) == ESP_OK && acknowledged == 1 && request == wlc::generator::kVersion &&
        nvs_get_blob(h, "gen_config", &candidate, &size) == ESP_OK &&
        size == sizeof(candidate) && wlc::generator::valid(candidate) && wlc::generator::total_ms(candidate) <= rf_limit_ms;
    esp_err_t ack_erased = nvs_erase_key(h, "rf_shield_ack");
    if (found) {
        esp_err_t erased = nvs_erase_key(h, "boot_generator");
        esp_err_t spectrum = nvs_erase_key(h, "boot_spectrum");
        valid = valid && ack_erased == ESP_OK && erased == ESP_OK && (spectrum == ESP_OK || spectrum == ESP_ERR_NVS_NOT_FOUND);
        if (nvs_commit(h) != ESP_OK) valid = false;
    }
    if (!found) nvs_commit(h);
    nvs_close(h);
    if (valid) config = candidate;
    return valid;
}

// ---- Local spectrum: the touch launcher's Spectrum tile -------------------------------------
// The launcher sets wlc/boot_spectrum in the shared NVS partition and boots this image. Frames
// are then drawn on the LCD; the first line from a USB host hands spectrum back to the host.
static bool local_spectrum;
// A view is one to three frequency ranges side by side. Captures are tuned every 20 MHz across
// each range; SpectrumView keeps, per column, the capture farthest from its own DC.
struct SpectrumBand {
    const char* title; const char* axis;
    uint16_t ranges[3][2]; uint8_t range_count;
};
static const SpectrumBand SPECTRUM_BANDS[] = {
    {"2.4 GHZ  CH 1-13", "2402    2442    2482", {{2402, 2482}}, 1},
    {"5 GHZ  CH 36-64", "5170    5250    5330", {{5170, 5330}}, 1},
    {"5 GHZ  CH 100-144", "5490    5610    5730", {{5490, 5730}}, 1},
    {"5 GHZ  CH 149-165", "5735    5785    5835", {{5735, 5835}}, 1},
    {"5 GHZ  ALL", "36-64  100-144  149", {{5170, 5330}, {5490, 5730}, {5735, 5835}}, 3},
};
static constexpr unsigned SPECTRUM_BAND_COUNT = sizeof(SPECTRUM_BANDS) / sizeof(SPECTRUM_BANDS[0]);
static unsigned spectrum_band, spectrum_slice, spectrum_slice_count;
static uint16_t spectrum_slices[40];
static uint32_t local_frames, local_draws;

static void spectrum_view_start(unsigned band) {
    spectrum_band = band % SPECTRUM_BAND_COUNT;
    spectrum_slice = spectrum_slice_count = 0;
    const SpectrumBand& b = SPECTRUM_BANDS[spectrum_band];
    for (unsigned r = 0; r < b.range_count; r++)
        for (unsigned f = b.ranges[r][0]; f <= b.ranges[r][1] && spectrum_slice_count < 40; f += 20)
            spectrum_slices[spectrum_slice_count++] = f;
    wlc_lcd_spectrum_end();                    // a new view starts with an empty trace and waterfall
    wlc_lcd_spectrum_begin();
    wlc_lcd_spectrum_ranges(b.ranges, b.range_count);
}

extern "C" bool wlc_usb_send(const void* data, size_t size) {
    if (!local_spectrum) return burst_serial_send(data, size);
    // 28-byte SPC1 header, then 2^header[26] dB bins, then a CRC.
    const uint8_t* f = static_cast<const uint8_t*>(data);
    if (size >= 32 && !memcmp(f, "SPC1", 4)) {
        unsigned bins = 1u << f[26];
        if (28 + bins + 4 == size) { wlc_lcd_spectrum_frame(f + 28, bins, spectrum_slices[spectrum_slice]); local_frames++; }
    }
    return true;
}

// Returns the requested view + 1, or 0 for none. The launcher writes the view the user picked.
static uint8_t take_boot_request() {
    nvs_handle_t h;
    uint8_t want = 0;
    if (nvs_open("wlc", NVS_READWRITE, &h) != ESP_OK) return 0;
    if (nvs_get_u8(h, "boot_spectrum", &want) == ESP_OK) { nvs_erase_key(h, "boot_spectrum"); nvs_commit(h); }
    nvs_close(h);
    return want;
}

static void local_spectrum_step() {
    const SpectrumBand& band = SPECTRUM_BANDS[spectrum_band];
    char line[32];
    snprintf(line, sizeof(line), "FREQ %u", spectrum_slices[spectrum_slice]);
    wlc_sdr_dispatch(line);
    // 30 ms of 256-bin FFTs, 4 averaged per frame; frames land in wlc_usb_send above.
    snprintf(line, sizeof(line), "SPEC 30 1 4 0 0 256");
    wlc_sdr_dispatch(line);
    last_command_us = esp_timer_get_time();   // keep the spectrum lease alive
    if (++spectrum_slice < spectrum_slice_count) return;
    spectrum_slice = 0;
    wlc_lcd_spectrum_commit();
    wlc_lcd_spectrum_draw(band.title, band.axis);
    local_draws++;
}
// Excluding the bank from the heap lets the exclusive modes share it explicitly.
// Packets occupy its first 64 KiB; SDR IQ uses the second 64 KiB.
static_assert(POOL_BYTES <= 65536, "Packet pool exceeds shared SRAM half-bank");

static void reply(const char* text) { burst_serial_send(text, strlen(text)); }

static bool restore_sniffer() {
    if (!seen) return false;
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
    wlc_lcd_show_mode(LcdSnapshot::SPECTRUM);
    last_command_us = esp_timer_get_time();
    return true;
}

static bool enter_generator() {
    if (!rf_approved || !rf_session.active(millis())) return false;
    radio_gate.store(true);
    portENTER_CRITICAL(&ftmMux);
    bool busy = isRun || wantRun || ftmActive || ftmHasQueued;
    portEXIT_CRITICAL(&ftmMux);
    if (busy) { radio_gate.store(false); return false; }
    // The private PHY does not start packet TX reliably after normal Wi-Fi
    // teardown on this C5. A fresh boot works with the same scratch layout.
    generator_boot_budget = rf_session.remaining(millis());
    generator_boot_magic = GENERATOR_BOOT_MAGIC;
    generator_boot_inverse = ~GENERATOR_BOOT_MAGIC;
    reply("OK REBOOT\n");
    delay(20);
    esp_restart();
}

// This image lives in ota_1 beside the touch launcher in ota_0. Holding BOOT for 2 s goes back,
// through the same full reset the generator uses: a plain restart can stay in ROM after raw PHY mode.
constexpr gpio_num_t BOOT_BUTTON = GPIO_NUM_28;
static uint32_t boot_pressed_at;
static bool return_to_launcher() {
    const esp_partition_t* launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                               ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!launcher) return false;
    if (generator_mode) generator_stop();
    if (esp_ota_set_boot_partition(launcher) != ESP_OK) return false;
    reply("OK REBOOT\n");
    delay(20);
    generator_reset();
}
static void check_return_to_launcher() {
    uint32_t now = millis();
    if (gpio_get_level(BOOT_BUTTON) != 0) {
        if (boot_pressed_at && now - boot_pressed_at < 1000 && local_spectrum)
            spectrum_view_start(spectrum_band + 1);
        boot_pressed_at = 0;
        return;
    }
    if (!boot_pressed_at) { boot_pressed_at = now | 1; return; }
    if (now - boot_pressed_at >= 2000) return_to_launcher();
}

static void local_cancel() {
    wlc_lcd_generator_cancel_input();
    generator_stop(); local_run.stop();
}
static void local_begin_burst() {
    using namespace wlc::generator;
    const Config &c = local_run.config;
    if (!rf_approved || !rf_session.allows(millis(), c.workflow == Radar ? (uint32_t)((radar_duration_us(kRadar[c.radar_profile]) + 1000999) / 1000) : duration(c))) { local_cancel(); return; }
    if (c.workflow == Radar) {
        generator_tone = true; generator_channel_value = c.channel; generator_backoff = c.backoff_qdb;
        // Include a bounded setup allowance, not fictitious microsecond timing
        // derived from the millisecond scheduler. The worker owns actual edges.
        generator_deadline = esp_timer_get_time() + radar_duration_us(kRadar[c.radar_profile]) + 1000000;
        guard_arm((uint32_t)((generator_deadline - esp_timer_get_time()) / 1000));
        radar_burst = true;
        generator_active = wlc_radar_start(c.channel, c.backoff_qdb, c.radar_profile);
        if (!generator_active) { generator_stop(); local_run.fail(); }
        else local_run.deadline = (uint32_t)(generator_deadline / 1000);
        return;
    }
    GeneratorRequest request{c.tone != 0, channel_at(c, local_run.index), c.backoff_qdb, duration(c)};
    if (!generator_start(request)) local_run.fail();
    else local_run.deadline = (uint32_t)(generator_deadline / 1000);
}
static void local_generator_step() {
    using namespace wlc::generator;
    Action action = wlc_lcd_generator_input();
    if (!wlc_lcd_generator_healthy()) {
        if (local_run.phase != Phase::Error) { generator_stop(); local_run.fail(); }
    } else if (action == Action::Return) return_to_launcher();
    else if (action == Action::Stop) local_cancel();
    else if (action == Action::Start && rf_session.allows(millis(), total_ms(local_run.config)) && local_run.start(millis())) local_begin_burst();

    if (!rf_session.active(millis())) { if (local_run.running()) local_cancel(); rf_session.lock(); }
    if (radar_burst && local_run.phase == Phase::Active && !wlc_radar_running()) {
        bool failure = wlc_radar_failed(); generator_stop();
        if (failure) local_run.fail(); else local_run.deadline = millis();
    }
    if (local_run.due(millis())) {
        if (local_run.phase == Phase::Active) generator_stop();
        local_run.advance(millis());
        if (local_run.phase == Phase::Active) local_begin_burst();
    }
    wlc_lcd_generator_tick(local_run, rf_session.remaining(millis()));
    // Renew only after the entire local control pass completed. The independent
    // timer remains armed while touch/SPI/PHY work runs, including pulse gaps.
    if (local_run.running()) {
        if (local_run.phase == Phase::Active) {
            int64_t remaining = generator_deadline - esp_timer_get_time();
            guard_arm(remaining <= 0 ? 0 : remaining < 5000000 ? (uint32_t)((remaining + 999) / 1000) : 5000);
        } else guard_arm(5000);
    }
}

void setup() {
    // Neither Arduino Serial nor the SDK console may consume this USB endpoint.
    esp_log_level_set("*", ESP_LOG_NONE);
    burst_serial_init();
    boot_trace("BOOT setup\n");
    gpio_config_t boot_pin{};
    boot_pin.pin_bit_mask = 1ULL << BOOT_BUTTON;
    boot_pin.mode = GPIO_MODE_INPUT;
    boot_pin.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&boot_pin);
    WiFi.useStaticBuffers(true); // honor the bounded sdkconfig buffer counts
    const bool host_generator = generator_boot_magic == GENERATOR_BOOT_MAGIC &&
                                generator_boot_inverse == ~GENERATOR_BOOT_MAGIC;
    const uint32_t host_budget = generator_boot_budget;
    generator_boot_magic = generator_boot_inverse = generator_boot_budget = 0;
    if (nvs_flash_init() == ESP_OK) {
        rf_approved = wlc::rf::approved(&rf_limit_ms);
        local_generator = take_generator_request(local_run.config);
    }
    // Explicit host RTC entry wins; pending launcher requests are consumed.
    if (host_generator) local_generator = false;
    const bool boot_generator = (host_generator && rf_approved && host_budget > 0 && host_budget <= rf_limit_ms) || local_generator;
    if (boot_generator && !local_generator) rf_session.unlock(rf_approved, true, millis(), host_budget);
    if (local_generator) rf_session.unlock(rf_approved, true, millis(), rf_limit_ms);
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
        // Preserve all 512 dedup entries and the ISR's internal-memory access,
        // while moving this 8 KiB cache out of the statically constrained bank.
        seen = (BeaconSeen*)heap_caps_calloc(DEDUP_SLOTS, sizeof(BeaconSeen), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (seen) sniffer_setup();
        else { radio_gate.store(true); esp_read_mac(myMac, ESP_MAC_WIFI_STA); reply("ERR sniffer_cache_allocation\n"); }
    }
    boot_trace("BOOT radio initialized\n");
    wlc_lcd_init(); // Failure does not prevent the radio/USB firmware from operating.
    if (local_generator && !wlc_lcd_generator_begin()) local_run.fail();
    burst_serial_init();
    last_command_us = esp_timer_get_time();
    const uint8_t requested_view = (!boot_generator && seen) ? take_boot_request() : 0;
    if (requested_view && enter_spectrum()) {
        local_spectrum = wlc_lcd_spectrum_begin();
        if (!local_spectrum) restore_sniffer();
        else {
            // The default analog filter passes ~20 MHz; 48 MHz makes the middle 40 MHz flat.
            char bw[] = "BANDWIDTH 48";
            wlc_sdr_dispatch(bw);
            spectrum_view_start(requested_view - 1);
        }
    }
}

void loop() {
    char line[224];
    int status = burst_serial_poll_line(line, sizeof(line));
    if (status < 0) reply("ERR command_length\n");
    // SPECLOCAL? only reads; any other line means a host wants the radio.
    if (status > 0 && !strcmp(line, "SPECLOCAL?")) {
        char text[160];
        snprintf(text, sizeof(text), "SPECLOCAL {\"active\":%s,\"view\":%u,\"slices\":%u,\"frames\":%lu,\"sweeps\":%lu}\n",
                 local_spectrum ? "true" : "false", spectrum_band, spectrum_slice_count,
                 (unsigned long)local_frames, (unsigned long)local_draws);
        reply(text);
        status = 0;
    }
    if (status > 0 && local_spectrum) {
        local_spectrum = false;
        wlc_lcd_spectrum_end();
        char bw[] = "BANDWIDTH 0";   // hand the host the receiver's default filter
        wlc_sdr_dispatch(bw);
    }
    if (status > 0) {
        // Commands (especially Stop) preempt screenshots; never interleave a
        // command response into a partially streamed base64 row.
        if (wlc_lcd_generator_shot_cancel()) reply("\n");
        const bool lcd_command = !strcmp(line, "LCDINFO?") || !strncmp(line, "IDENTIFY", 8);
        if (!lcd_command && !local_generator && (!generator_mode || generator_heartbeat(line)))
            last_command_us = esp_timer_get_time();
        if (!strcmp(line, "RFAPPROVAL?")) {
            char text[200];
            snprintf(text, sizeof(text), "RFAPPROVAL {\"approved\":%s,\"unlocked\":%s,\"remaining_ms\":%lu,\"max_session_ms\":%lu}\n",
                rf_approved ? "true" : "false", rf_session.active(millis()) ? "true" : "false",
                (unsigned long)rf_session.remaining(millis()), (unsigned long)rf_limit_ms);
            reply(text);
        } else if (!strncmp(line, "RFAPPROVE ", 10)) {
            if (generator_mode || generator_active || local_generator) reply("ERR busy\n");
            else if (!wlc::rf::install(line + 10)) reply("ERR approval_invalid\n");
            else { rf_approved = wlc::rf::approved(&rf_limit_ms); rf_session.lock(); reply("OK\n"); }
        } else if (!strcmp(line, "RFUNLOCK SHIELDED")) {
            reply(!spectrum_mode && !local_generator && rf_session.unlock(rf_approved, true, millis(), rf_limit_ms) ? "OK\n" : "ERR approval_or_session\n");
        } else if (!strcmp(line, "RFLOCK") || !strcmp(line, "RFREVOKE")) {
            if (local_generator) local_cancel(); else generator_stop();
            rf_session.lock();
            if (!strcmp(line, "RFREVOKE")) {
                bool removed = wlc::rf::forget(); rf_approved = wlc::rf::approved(&rf_limit_ms);
                reply(removed ? "OK\n" : "ERR approval_storage\n");
            } else reply("OK\n");
        } else if (!strcmp(line, "RADARINFO?")) {
            char text[384]; wlc_radar_info(text, sizeof(text)); reply("RADARINFO "); reply(text); reply("\n");
        } else if (!strcmp(line, "GENLOCAL?")) {
            char text[384];
            snprintf(text, sizeof(text), "GENLOCAL {\"local\":%s,\"phase\":\"%s\",\"workflow\":%lu,\"index\":%lu,\"count\":%lu,\"channel\":%lu,\"remaining_ms\":%lu,\"touch\":%s}\n",
                local_generator ? "true" : "false", wlc::generator::phase_name(local_run.phase),
                (unsigned long)local_run.config.workflow, (unsigned long)local_run.index,
                (unsigned long)wlc::generator::steps(local_run.config),
                (unsigned long)wlc::generator::channel_at(local_run.config, local_run.index),
                (unsigned long)local_run.remaining(millis()), wlc_lcd_generator_healthy() ? "true" : "false");
            reply(text);
        } else if (!strcmp(line, "LCDSHOT?")) {
            if (!local_generator || !wlc_lcd_generator_shot()) reply("ERR screenshot_unavailable\n");
        } else if (!strncmp(line, "LCDTAP ", 7)) {
            const char *p = line + 7; uint32_t x, y;
            if (!command_number(p, x) || !command_number(p, y) || !command_end(p) || x > 319 || y > 239)
                reply("ERR tap_args\n");
            else reply(local_generator && wlc_lcd_generator_tap(x, y) ? "OK\n" : "ERR tap_unavailable\n");
        } else if (!strcmp(line, "LCDINFO?")) {
            char text[256]; wlc_lcd_info(text, sizeof(text));
            reply("LCDINFO "); reply(text); reply("\n");
        } else if (!strncmp(line, "IDENTIFY", 8)) {
            uint32_t number, seconds;
            if (generator_active || local_generator) reply("ERR busy\n");
            else if (!lcd_usb_identify(line, number, seconds)) reply("ERR identify_args\n");
            else reply(wlc_lcd_identify(number, seconds) ? "OK\n" : "ERR display_unavailable\n");
        } else if (!strcmp(line, "LAUNCHER")) {
            if (!return_to_launcher()) reply("ERR launcher\n");
        } else if (!strcmp(line, "MODE?")) {
            reply(generator_mode ? "MODE GENERATOR\n" : spectrum_mode ? "MODE SPECTRUM\n" : "MODE SNIFFER\n");
        } else if (!strcmp(line, "TXINFO?")) {
            char text[512];
            snprintf(text, sizeof(text), "TXINFO {\"mode\":\"%s\",\"active\":%s,\"waveform\":\"%s\",\"channel\":%u,\"backoff_qdb\":%u,\"remaining_ms\":%lld,\"max_duration_ms\":30000,\"lease_ms\":5000,\"channels\":[1,2,3,4,5,6,7,8,9,10,11,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165],\"power_calibrated\":false}\n",
                generator_mode ? "generator" : spectrum_mode ? "spectrum" : "sniffer",
                generator_active ? "true" : "false", generator_tone ? "cw" : "packet",
                (unsigned)generator_channel_value, (unsigned)generator_backoff,
                (long long)(generator_active && generator_deadline > esp_timer_get_time() ?
                    (generator_deadline - esp_timer_get_time()) / 1000 : 0));
            reply(text);
        } else if (!strcmp(line, "PING")) {
            reply("OK\n");
        } else if (!strcmp(line, "TXSTOP")) {
            if (local_generator) local_cancel(); else generator_stop();
            reply("OK\n");
        } else if (!strncmp(line, "TX ", 3)) {
            GeneratorRequest request;
            if (!generator_mode) reply("ERR generator_mode_required\n");
            else if (!generator_request(line, request)) reply("ERR generator_args\n");
            else if (generator_active || local_generator) reply("ERR busy\n");
            else if (!rf_approved || !rf_session.allows(millis(), request.duration_ms)) reply("ERR rf_locked\n");
            else {
                wlc_lcd_show_mode(LcdSnapshot::TRANSMITTING);
                last_command_us = esp_timer_get_time();
                bool started = generator_start(request);
                reply(started ? "OK\n" : "ERR generator_allocation\n");
            }
        } else if (!strcmp(line, "MODE GENERATOR")) {
            if (!rf_approved) reply("ERR approval_required\n");
            else if (!rf_session.active(millis())) reply("ERR rf_locked\n");
            else if (spectrum_mode) reply("ERR busy\n");
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
                "WLCINFO {\"firmware\":\"airhorn-dev\",\"generator_lease\":2,\"rf_approval\":1,\"mode\":\"%s\",\"run\":%s,\"ftm\":%s,\"heap\":%u,\"largest_internal\":%u,\"ble_initialized\":%s,\"shared_packet_pool\":true,\"fft_max\":512,\"bands_mhz\":[[2400,2483],[5150,5895]],\"board\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",
                generator_mode ? "generator" : spectrum_mode ? "spectrum" : "sniffer", isRun ? "true" : "false",
                ftmActive ? "true" : "false", (unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                NimBLEDevice::isInitialized() ? "true" : "false",
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
            reply(text);
        } else if (!strcmp(line, "CAPS")) {
            reply("CAPS SPEC SPECN SPECCAPS SPECSTAT DCT RXLIMITS GAIN HWAGC WLCMODE WLCGENERATOR WLCDISPLAY WLCLAUNCHER WLCGENLOCAL\n");
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
    if (generator_mode && !rf_session.active(millis())) {
        if (local_generator && local_run.running()) local_cancel();
        else if (generator_active) generator_stop();
        rf_session.lock();
    }
    if (status > 0 && generator_mode && generator_active && !local_generator) {
        uint32_t remaining = generator_guard_remaining(esp_timer_get_time(), last_command_us, generator_deadline);
        if (remaining) guard_arm(remaining);
    }
    if (generator_mode && generator_active && !local_generator && esp_timer_get_time() >= generator_deadline)
        generator_stop();
    if (!local_generator && (spectrum_mode || generator_mode) && esp_timer_get_time() - last_command_us > 5000000) {
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
    if (local_spectrum && spectrum_mode) local_spectrum_step();
    if (local_generator) local_generator_step();
    if (!generator_active && !local_spectrum && !local_generator) {
        LcdSnapshot state{};
        state.mode = generator_mode ? LcdSnapshot::GENERATOR : spectrum_mode ? LcdSnapshot::SPECTRUM :
            ftmActive ? LcdSnapshot::RANGING : isRun ? LcdSnapshot::CAPTURE : LcdSnapshot::IDLE;
        state.now = millis();
        state.captured = captured; state.forwarded = lcd_ble_forwarded;
        state.ring_drops = lcd_ring_drops; state.oversize_drops = odrop; state.ble_drops = bdrop;
        state.retries = nfail; state.dedup = ddrop;
        state.pool_used = pool ? pool_used() : 0; state.pool_peak = poolmax; state.pool_bytes = POOL_BYTES;
        state.channel_failures = chfail; state.channel = cur_ch; state.width = wantWidth;
        state.channel_count = activeN; memcpy(state.channels, activeCh, activeN);
        memcpy(state.mac, myMac, 6); state.connected = connH != 0xffff; state.subscribed = subscribed;
        wlc_lcd_tick(state);
        wlc_lcd_led_override(state.now);
    }
    check_return_to_launcher();
    wlc_lcd_generator_shot_tick();
    delay(1);
}
