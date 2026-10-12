// SPDX-License-Identifier: MIT
// Line commands on the USB-Serial/JTAG console, answering one JSON object per line. This is the
// test harness's way in: everything a tap can start, a script can start and measure.
#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wlc_radio.hpp"
#include "switch_app.hpp"
#include "airhorn_app.hpp"
#include "rf_approval.hpp"
#include <cerrno>
#include <climits>
#include "ui_remote.hpp"

namespace wlc::apps {
namespace {

CliHook g_hook;

bool parse_channels(char *list, radio::Plan &plan)
{
    plan.channel_count = 0;
    for (char *tok = strtok(list, ","); tok && plan.channel_count < radio::kMaxChannels; tok = strtok(nullptr, ",")) {
        int ch = atoi(tok);
        if (ch < 1 || ch > 177) return false;
        plan.channels[plan.channel_count++] = (uint8_t)ch;
    }
    return plan.channel_count > 0;
}

void handle(char *line)
{
    char *cmd = strtok(line, " ");
    if (!cmd) return;
    if (!strcmp(cmd, "heap")) {
        printf("{\"heap\":{\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u,\"min_internal\":%u}}\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    } else if (!strcmp(cmd, "radio")) {
        char *sub = strtok(nullptr, " ");
        if (sub && !strcmp(sub, "start")) {
            radio::Plan plan{};
            char *list = strtok(nullptr, " ");
            char *dwell = strtok(nullptr, " ");
            char *data = strtok(nullptr, " ");
            if (!list || !parse_channels(list, plan)) { printf("{\"error\":\"channels\"}\n"); return; }
            plan.dwell_ms = dwell ? (uint16_t)atoi(dwell) : 300;
            plan.data_frames = data && !strcmp(data, "data");
            esp_err_t err = radio::start(plan);
            printf("{\"radio_start\":\"%s\"}\n", esp_err_to_name(err));
        } else if (sub && !strcmp(sub, "plan")) {
            radio::Plan p = radio::current_plan();
            printf("{\"plan\":{\"dwell_ms\":%u,\"data\":%s,\"channels\":[", p.dwell_ms, p.data_frames ? "true" : "false");
            for (int i = 0; i < p.channel_count; i++) printf("%s%u", i ? "," : "", p.channels[i]);
            printf("]}}\n");
        } else if (sub && !strcmp(sub, "stop")) {
            printf("{\"radio_stop\":\"%s\"}\n", esp_err_to_name(radio::stop()));
        } else {
            radio::Stats s = radio::stats();
            printf("{\"radio\":{\"running\":%s,\"channel\":%u,\"captured\":%lu,\"ring_drops\":%lu,"
                   "\"oversize\":%lu,\"hops\":%lu,\"hop_failures\":%lu,\"queries\":%lu,\"query_failures\":%lu}}\n",
                   s.running ? "true" : "false", s.channel, (unsigned long)s.captured,
                   (unsigned long)s.ring_drops, (unsigned long)s.oversize, (unsigned long)s.hops,
                   (unsigned long)s.hop_failures, (unsigned long)s.queries_sent, (unsigned long)s.query_failures);
        }
    } else if (!strcmp(cmd, "screenshot")) {
        ui_remote_screenshot();
    } else if (!strcmp(cmd, "tap") || !strcmp(cmd, "hold")) {
        char *x = strtok(nullptr, " "), *y = strtok(nullptr, " ");
        if (!x || !y) { printf("{\"error\":\"tap x y\"}\n"); return; }
        ui_remote_tap(atoi(x), atoi(y), !strcmp(cmd, "hold") ? 1500 : 80);
    } else if (!strcmp(cmd, "swipe")) {
        int v[4];
        for (int &n : v) { char *t = strtok(nullptr, " "); if (!t) { printf("{\"error\":\"swipe x0 y0 x1 y1\"}\n"); return; } n = atoi(t); }
        ui_remote_swipe(v[0], v[1], v[2], v[3], 300);
    } else if (!strcmp(cmd, "rfapproval")) {
        printf("{\"rf_approval\":{\"approved\":%s,\"max_session_ms\":900000}}\n", rf::approved() ? "true" : "false");
    } else if (!strcmp(cmd, "rfapprove")) {
        char *permit = strtok(nullptr, " ");
        bool ok = permit && !strtok(nullptr, " ") && rf::install(permit);
        printf("{\"rf_approval_installed\":%s}\n", ok ? "true" : "false");
    } else if (!strcmp(cmd, "rfrevoke")) {
        printf("{\"rf_approval_removed\":%s}\n", rf::forget() ? "true" : "false");
    } else if (!strcmp(cmd, "generator")) {
        auto config = generator_config();
        char *arg = strtok(nullptr, " ");
        if (arg) {
            uint32_t *values[] = {&config.workflow, &config.tone, &config.band5, &config.channel,
                &config.backoff_qdb, &config.single_ms, &config.on_ms, &config.off_ms, &config.count, &config.dwell_ms, &config.radar_profile};
            for (auto *value : values) {
                if (!arg || arg[0] < '0' || arg[0] > '9') { printf("{\"error\":\"generator_args\"}\n"); return; }
                char *end; errno = 0; unsigned long number = strtoul(arg, &end, 10);
                if (*end || errno || number > UINT32_MAX) { printf("{\"error\":\"generator_args\"}\n"); return; }
                *value = number; arg = strtok(nullptr, " ");
            }
            if (arg || !generator::valid(config) || !save_generator_config(config)) { printf("{\"error\":\"generator_config\"}\n"); return; }
        }
        printf("{\"generator\":{\"workflow\":%lu,\"tone\":%lu,\"band5\":%lu,\"channel\":%lu,\"backoff_qdb\":%lu,\"single_ms\":%lu,\"on_ms\":%lu,\"off_ms\":%lu,\"count\":%lu,\"dwell_ms\":%lu,\"radar_profile\":%lu}}\n",
            (unsigned long)config.workflow, (unsigned long)config.tone, (unsigned long)config.band5,
            (unsigned long)config.channel, (unsigned long)config.backoff_qdb, (unsigned long)config.single_ms,
            (unsigned long)config.on_ms, (unsigned long)config.off_ms, (unsigned long)config.count, (unsigned long)config.dwell_ms, (unsigned long)config.radar_profile);
    } else if (!strcmp(cmd, "boot")) {
        char *target = strtok(nullptr, " ");
        bool generator = target && !strcmp(target, "generator");
        bool spectrum = target && !strcmp(target, "spectrum");
        char *view = strtok(nullptr, " ");
        if (!target || (!spectrum && !generator && strcmp(target, "sensor"))) { printf("{\"error\":\"boot sensor|spectrum [view]|generator shielded\"}\n"); return; }
        if (generator && (!wlc::rf::approved() || !view || strcmp(view, "shielded"))) { printf("{\"error\":\"rf_approval_and_shielding_ack_required\"}\n"); return; }
        printf("{\"boot\":\"%s\"}\n", target);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!(generator ? boot_generator_image(generator_config(), view && !strcmp(view, "shielded")) : boot_sensor_image(spectrum ? (view ? atoi(view) : 0) : -1))) printf("{\"error\":\"no_sensor_image\"}\n");
    } else if (!(g_hook && g_hook(cmd, line + strlen(cmd) + 1))) {
        printf("{\"error\":\"unknown\",\"command\":\"%s\"}\n", cmd);
    }
    fflush(stdout);
}

void cli_task(void *)
{
    char line[160];
    size_t n = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r') continue;
        if (c == '\n') { line[n] = 0; handle(line); n = 0; continue; }
        if (n < sizeof(line) - 1) line[n++] = (char)c;
    }
}

}  // namespace

void start_cli(CliHook hook)
{
    g_hook = hook;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, nullptr, _IONBF, 0);
    xTaskCreate(cli_task, "wlc_cli", 8192, nullptr, 3, nullptr);
}

}
