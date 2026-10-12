// SPDX-License-Identifier: MIT
#include "services.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wlc_radio.hpp"
#include "wlc_apps.hpp"
#include "cli.hpp"

namespace wlc::apps {
namespace {

const char *TAG = "wlc_services";

class MutexLock final : public wlc::Lock {
public:
    MutexLock() : m_(xSemaphoreCreateMutex()) {}
    void lock() override { xSemaphoreTake(m_, portMAX_DELAY); }
    void unlock() override { xSemaphoreGive(m_); }
private:
    SemaphoreHandle_t m_;
};

wlc::Survey g_survey;
MutexLock *g_lock;

void *psram_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void on_frame(const uint8_t *f, size_t len, int8_t rssi, uint8_t ch, bool is_5ghz, uint32_t now)
{
    g_survey.ingest(f, len, wlc::RxMeta{rssi, ch, is_5ghz ? wlc::Band::B5G : wlc::Band::B2G4, now});
}

size_t build_query(uint8_t *out, size_t cap, uint8_t token, int set)
{
    return wlc::build_anqp_query_body(out, cap, token, set ? wlc::QuerySet::Location : wlc::QuerySet::Passpoint);
}

// ---- console commands: the same data the screens show, as JSON lines -----------------------

void print_aps(size_t max, bool passpoint_only)
{
    auto *rows = static_cast<wlc::ApInfo *>(psram_alloc(sizeof(wlc::ApInfo) * max));
    if (!rows) { printf("{\"error\":\"no_mem\"}\n"); return; }
    size_t total = 0;
    size_t n = g_survey.aps(rows, max, wlc::ApSort::Rssi, wlc::BAND_ALL, passpoint_only, &total);
    printf("{\"aps\":{\"total\":%u,\"rows\":[", (unsigned)total);
    for (size_t i = 0; i < n; i++) {
        const auto &a = rows[i];
        char mac[18], ssid[80];
        wlc::mac_to_string(a.bssid, mac);
        wlc::ssid_to_string(a.ssid, a.ssid_len, ssid, sizeof(ssid));
        // SSIDs may hold quotes or backslashes; neutralise them rather than build an escaper.
        for (char *c = ssid; *c; c++) if (*c == '"' || *c == '\\') *c = '?';
        printf("%s{\"bssid\":\"%s\",\"ssid\":\"%s\",\"hidden\":%s,\"band\":\"%s\",\"ch\":%u,\"width\":\"%s\","
               "\"rssi\":%d,\"avg\":%d,\"sec\":\"%s\",\"phy\":\"%s\",\"beacons\":%lu,\"passpoint\":%s,\"anqp\":%s}",
               i ? "," : "", mac, ssid, a.hidden ? "true" : "false", wlc::band_label(a.band), a.channel,
               wlc::width_label(a.width), a.rssi_last, a.rssi_avg, wlc::security_label(a.security, a.suite_b),
               wlc::phy_label(a.phy), (unsigned long)a.beacons, a.passpoint ? "true" : "false",
               a.has_anqp ? "true" : "false");
    }
    printf("]}}\n");
    free(rows);
}

void print_clients(size_t max)
{
    auto *rows = static_cast<wlc::ClientInfo *>(psram_alloc(sizeof(wlc::ClientInfo) * max));
    if (!rows) { printf("{\"error\":\"no_mem\"}\n"); return; }
    size_t total = 0;
    size_t n = g_survey.clients(rows, max, wlc::ClientSort::Rssi, nullptr, &total);
    printf("{\"clients\":{\"total\":%u,\"rows\":[", (unsigned)total);
    for (size_t i = 0; i < n; i++) {
        const auto &c = rows[i];
        char mac[18], bssid[18];
        wlc::mac_to_string(c.mac, mac);
        wlc::mac_to_string(c.bssid, bssid);
        printf("%s{\"mac\":\"%s\",\"random\":%s,\"rssi\":%d,\"frames\":%lu,\"assoc\":%s%s%s,\"probes\":%u,\"wildcard\":%lu}",
               i ? "," : "", mac, c.randomized ? "true" : "false", c.rssi_last, (unsigned long)c.frames,
               c.associated ? "\"" : "", c.associated ? bssid : "null", c.associated ? "\"" : "",
               c.n_probed, (unsigned long)c.wildcard_probes);
    }
    printf("]}}\n");
    free(rows);
}

bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

void print_anqp(const char *arg)
{
    uint8_t bssid[6];
    auto *rec = static_cast<wlc::AnqpRecord *>(psram_alloc(sizeof(wlc::AnqpRecord)));
    if (!rec) { printf("{\"error\":\"no_mem\"}\n"); return; }
    if (!parse_mac(arg, bssid) || !g_survey.anqp_for(bssid, rec)) {
        printf("{\"anqp\":null}\n");
        free(rec);
        return;
    }
    printf("{\"anqp\":{\"present\":%lu,\"responses\":%u,\"domains\":[", (unsigned long)rec->present, rec->responses);
    for (int i = 0; i < rec->n_domains; i++) printf("%s\"%s\"", i ? "," : "", rec->domains[i].s);
    printf("],\"realms\":[");
    for (int i = 0; i < rec->n_nai_realms; i++) printf("%s\"%s\"", i ? "," : "", rec->nai_realms[i].realm.s);
    printf("],\"ois\":[");
    for (int i = 0; i < rec->n_roaming_ois; i++) { char oi[31]; printf("%s\"%s\"", i ? "," : "", wlc::oi_to_string(rec->roaming_ois[i], oi)); }
    printf("],\"venues\":[");
    for (int i = 0; i < rec->n_venue_names; i++) printf("%s\"%s\"", i ? "," : "", rec->venue_names[i].name.s);
    printf("],\"operators\":[");
    for (int i = 0; i < rec->n_operator_names; i++) printf("%s\"%s\"", i ? "," : "", rec->operator_names[i].name.s);
    printf("],\"plmns\":[");
    for (int i = 0; i < rec->n_plmns; i++) printf("%s\"%s-%s\"", i ? "," : "", rec->plmns[i].mcc, rec->plmns[i].mnc);
    printf("],\"civic\":\"%s\",\"geo\":\"%s\"}}\n", rec->civic_summary.s, rec->geo_summary.s);
    free(rec);
}

bool cli_hook(const char *cmd, char *args)
{
    char *arg = strtok(args, " ");
    if (!strcmp(cmd, "aps")) print_aps(arg ? atoi(arg) : 20, false);
    else if (!strcmp(cmd, "hs20")) print_aps(arg ? atoi(arg) : 24, true);
    else if (!strcmp(cmd, "clients")) print_clients(arg ? atoi(arg) : 20);
    else if (!strcmp(cmd, "anqp")) print_anqp(arg);
    else if (!strcmp(cmd, "passpoint")) printf("{\"passpoint_targets\":%u}\n", (unsigned)refresh_passpoint_targets());
    else if (!strcmp(cmd, "survey")) {
        if (arg && !strcmp(arg, "reset")) g_survey.reset();
        wlc::Stats s = g_survey.stats();
        printf("{\"survey\":{\"frames\":%lu,\"aps\":%u,\"clients\":%u,\"malformed\":%lu,\"gas_responses\":%lu,"
               "\"gas_dropped\":%lu,\"anqp_completed\":%lu,\"pending\":%u,\"ap_evictions\":%lu}}\n",
               (unsigned long)s.frames, s.aps, s.clients, (unsigned long)s.malformed,
               (unsigned long)s.gas_responses, (unsigned long)s.gas_dropped, (unsigned long)s.anqp_completed,
               s.pending, (unsigned long)s.ap_evictions);
    } else return false;
    return true;
}

}  // namespace

const uint8_t kAllChannels[] = {1, 6, 11, 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116,
                                120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
const uint8_t kAllChannelCount = sizeof(kAllChannels);

wlc::Survey &survey() { return g_survey; }

size_t refresh_passpoint_targets(bool include_interworking)
{
    // 24 rows are ~4 KB: heap, not the caller's stack (the console task overflowed on it).
    auto *rows = static_cast<wlc::ApInfo *>(psram_alloc(sizeof(wlc::ApInfo) * radio::kMaxTargets));
    if (!rows) return 0;
    size_t n = g_survey.aps(rows, radio::kMaxTargets, wlc::ApSort::Rssi, wlc::BAND_2G4 | wlc::BAND_5G, true, nullptr, include_interworking);
    radio::Target targets[radio::kMaxTargets];
    for (size_t i = 0; i < n; i++) {
        memcpy(targets[i].bssid, rows[i].bssid, 6);
        targets[i].channel = rows[i].rx_channel ? rows[i].rx_channel : rows[i].channel;
    }
    free(rows);
    radio::set_targets(targets, n, 4000);
    return n;
}

uint32_t rssi_color(int8_t rssi)
{
    if (rssi >= -55) return 0xe53935;   // red: strongest
    if (rssi >= -67) return 0xfb8c00;
    if (rssi >= -75) return 0xfdd835;
    if (rssi >= -85) return 0x43a5f5;
    return 0x607d8b;
}

esp_err_t start_services()
{
    g_lock = new MutexLock();
    wlc::Config cfg;
    cfg.alloc = psram_alloc;
    cfg.dealloc = free;
    cfg.lock = g_lock;
    if (!g_survey.init(cfg)) ESP_LOGE(TAG, "survey tables did not allocate");
    else ESP_LOGI(TAG, "survey tables: %u bytes in PSRAM", (unsigned)wlc::Survey::memory_required(cfg));
    esp_err_t err = radio::init(on_frame, build_query);
    start_cli(cli_hook);
    return err;
}

}
