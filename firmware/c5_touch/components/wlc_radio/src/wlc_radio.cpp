// SPDX-License-Identifier: MIT
#include "wlc_radio.hpp"

#include <atomic>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace wlc::radio {
namespace {

const char *TAG = "wlc_radio";
// In PSRAM: internal SRAM dipped to 16 KB free during capture with the ring there.
constexpr size_t kRingBytes = 48 * 1024;
constexpr size_t kMaxFrame = 1600;
constexpr uint32_t kAnyWait = 250;   // ms the driver may hold a GAS request on-channel

struct Header {
    uint32_t now_ms;
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
};

FrameSink g_sink;
QueryBuilder g_builder;
RingbufHandle_t g_ring;
SemaphoreHandle_t g_lock;           // guards g_plan / g_targets against the radio task
TaskHandle_t g_task;
Plan g_plan;
Target g_targets[kMaxTargets];
uint32_t g_last_query[kMaxTargets];
uint8_t g_query_set[kMaxTargets];
size_t g_target_count;
uint32_t g_query_interval_ms = 4000;
uint8_t g_mac[6];
uint8_t g_token = 1;
std::atomic<bool> g_running{false};
std::atomic<uint8_t> g_channel{1};
std::atomic<uint32_t> g_captured, g_ring_drops, g_oversize, g_hops, g_hop_failures,
    g_queries, g_query_failures;

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Runs in the Wi-Fi driver's task: copy and leave. Parsing happens on the radio task.
void IRAM_ATTR on_frame(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL) return;
    const auto *p = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    int len = (int)p->rx_ctrl.sig_len - 4;           // sig_len includes the FCS
    if (len < 10) return;
    g_captured++;
    // Data frames are only wanted for addressing; their payload is never read.
    if (type == WIFI_PKT_DATA && len > 64) len = 64;
    if (len > (int)kMaxFrame) { g_oversize++; return; }
    void *slot = nullptr;
    if (xRingbufferSendAcquire(g_ring, &slot, sizeof(Header) + len, 0) != pdTRUE) {
        g_ring_drops++;
        return;
    }
    Header h{now_ms(), (int8_t)p->rx_ctrl.rssi, (uint8_t)p->rx_ctrl.channel, (uint16_t)len};
    memcpy(slot, &h, sizeof(h));
    memcpy(static_cast<uint8_t *>(slot) + sizeof(h), p->payload, len);
    xRingbufferSendComplete(g_ring, slot);
}

void send_query(const Target &t, uint8_t &set)
{
    uint8_t buf[sizeof(wifi_action_tx_req_t) + 128] = {};
    auto *req = reinterpret_cast<wifi_action_tx_req_t *>(buf);
    size_t n = g_builder ? g_builder(req->data, 128, g_token, set) : 0;
    if (!n) return;
    if (++g_token == 0) g_token = 1;
    req->ifx = WIFI_IF_STA;
    memcpy(req->dest_mac, t.bssid, 6);
    req->type = WIFI_OFFCHAN_TX_REQ;
    req->channel = t.channel;
    req->sec_channel = WIFI_SECOND_CHAN_NONE;
    req->wait_time_ms = kAnyWait;
    req->no_ack = false;
    req->rx_cb = nullptr;              // the promiscuous path already sees the reply
    req->data_len = n;
    if (esp_wifi_action_tx_req(req) == ESP_OK) { g_queries++; set ^= 1; }
    else g_query_failures++;
}

void radio_task(void *)
{
    size_t index = 0;
    uint32_t next_hop = 0, next_poke = 0;
    for (;;) {
        size_t size = 0;
        auto *item = static_cast<uint8_t *>(xRingbufferReceive(g_ring, &size, pdMS_TO_TICKS(20)));
        if (item) {
            Header h;
            memcpy(&h, item, sizeof(h));
            if (g_sink) g_sink(item + sizeof(h), h.len, h.rssi, h.channel, h.channel > 14, h.now_ms);
            vRingbufferReturnItem(g_ring, item);
        }
        if (!g_running) continue;
        uint32_t now = now_ms();
        xSemaphoreTake(g_lock, portMAX_DELAY);
        if (g_plan.channel_count > 1 && (int32_t)(now - next_hop) >= 0) {
            index = (index + 1) % g_plan.channel_count;
            uint8_t ch = g_plan.channels[index];
            if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) g_channel = ch;
            else g_hop_failures++;
            g_hops++;
            next_hop = now + g_plan.dwell_ms;
        }
        // One due target per check, as the sensor firmware does, so pokes barely dent capture.
        if (g_target_count && (int32_t)(now - next_poke) >= 0) {
            next_poke = now + 100;
            for (size_t i = 0; i < g_target_count; i++) {
                if (g_targets[i].channel == g_channel &&
                    now - g_last_query[i] >= g_query_interval_ms) {
                    send_query(g_targets[i], g_query_set[i]);
                    g_last_query[i] = now;
                    break;
                }
            }
        }
        xSemaphoreGive(g_lock);
    }
}

}  // namespace

esp_err_t init(FrameSink sink, QueryBuilder builder)
{
    g_sink = sink;
    g_builder = builder;
    g_lock = xSemaphoreCreateMutex();
    g_ring = xRingbufferCreateWithCaps(kRingBytes, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_lock || !g_ring) return ESP_ERR_NO_MEM;

#define RETURN_ON_ERR(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { \
        ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(e_)); return e_; } } while (0)
    RETURN_ON_ERR(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    RETURN_ON_ERR(esp_wifi_init(&cfg));
    RETURN_ON_ERR(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // STA, never associated: action-frame TX needs an interface, and nothing here connects.
    RETURN_ON_ERR(esp_wifi_set_mode(WIFI_MODE_STA));
    RETURN_ON_ERR(esp_wifi_start());
    // Band mode is only accepted once the driver has started.
    RETURN_ON_ERR(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, g_mac);
    RETURN_ON_ERR(esp_wifi_set_promiscuous_rx_cb(&on_frame));
#undef RETURN_ON_ERR
    if (xTaskCreate(radio_task, "wlc_radio", 6144, nullptr, 5, &g_task) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready, mac %02x:%02x:%02x:%02x:%02x:%02x",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    return ESP_OK;
}

esp_err_t start(const Plan &plan)
{
    if (!plan.channel_count || plan.channel_count > kMaxChannels) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_plan = plan;
    if (g_plan.dwell_ms < 50) g_plan.dwell_ms = 50;
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL |
                       (plan.data_frames ? WIFI_PROMIS_FILTER_MASK_DATA : 0u)};
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err == ESP_OK) err = esp_wifi_set_channel(plan.channels[0], WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) { g_channel = plan.channels[0]; err = esp_wifi_set_promiscuous(true); }
    g_running = err == ESP_OK;
    xSemaphoreGive(g_lock);
    return err;
}

esp_err_t stop()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_running = false;
    g_target_count = 0;
    esp_err_t err = esp_wifi_set_promiscuous(false);
    xSemaphoreGive(g_lock);
    return err;
}

esp_err_t set_targets(const Target *targets, size_t count, uint32_t interval_ms)
{
    if (count > kMaxTargets) count = kMaxTargets;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (count) memcpy(g_targets, targets, count * sizeof(Target));
    uint32_t now = now_ms();
    for (size_t i = 0; i < count; i++) { g_last_query[i] = now - interval_ms; g_query_set[i] = 0; }
    g_target_count = count;
    g_query_interval_ms = interval_ms ? interval_ms : 4000;
    xSemaphoreGive(g_lock);
    return ESP_OK;
}

Stats stats()
{
    return Stats{g_captured, g_ring_drops, g_oversize, g_hops, g_hop_failures, g_queries,
                 g_query_failures, g_channel, g_running};
}

Plan current_plan()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Plan p = g_plan;
    xSemaphoreGive(g_lock);
    return p;
}

void own_mac(uint8_t out[6]) { memcpy(out, g_mac, 6); }

}  // namespace wlc::radio
