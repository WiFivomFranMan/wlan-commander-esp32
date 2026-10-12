// SPDX-License-Identifier: MIT
// Test hooks for the USB console: a screenshot of what LVGL is showing, and a virtual pointer
// that taps like a finger. Lets a script see and drive the UI on a board nobody is looking at.
#include "ui_remote.hpp"

#include <cstdio>
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

namespace wlc::apps {
namespace {

lv_indev_t *g_pointer;
volatile int16_t g_x, g_y;
volatile bool g_pressed;

void read_pointer(lv_indev_t *, lv_indev_data_t *data)
{
    data->point.x = g_x;
    data->point.y = g_y;
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Composites the active screen with the top layer, where Brookesia draws its bars and overlays.
lv_draw_buf_t *capture()
{
    lv_draw_buf_t *screen = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    return screen;
}

}  // namespace

void ui_remote_init()
{
    if (bsp_display_lock(1000) != ESP_OK) return;
    g_pointer = lv_indev_create();
    lv_indev_set_type(g_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_pointer, read_pointer);
    bsp_display_unlock();
}

void ui_remote_tap(int x, int y, int hold_ms)
{
    g_x = (int16_t)x;
    g_y = (int16_t)y;
    g_pressed = true;
    vTaskDelay(pdMS_TO_TICKS(hold_ms > 30 ? hold_ms : 30));
    g_pressed = false;
    vTaskDelay(pdMS_TO_TICKS(60));
    printf("{\"tap\":[%d,%d]}\n", x, y);
}

void ui_remote_swipe(int x0, int y0, int x1, int y1, int ms)
{
    const int steps = 12;
    g_x = (int16_t)x0; g_y = (int16_t)y0; g_pressed = true;
    for (int i = 1; i <= steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(ms / steps));
        g_x = (int16_t)(x0 + (x1 - x0) * i / steps);
        g_y = (int16_t)(y0 + (y1 - y0) * i / steps);
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    g_pressed = false;
    vTaskDelay(pdMS_TO_TICKS(60));
    printf("{\"swipe\":[%d,%d,%d,%d]}\n", x0, y0, x1, y1);
}

void ui_remote_screenshot()
{
    if (bsp_display_lock(2000) != ESP_OK) { printf("{\"error\":\"display_lock\"}\n"); return; }
    lv_draw_buf_t *buf = capture();
    bsp_display_unlock();
    if (!buf) { printf("{\"error\":\"snapshot\"}\n"); return; }
    const uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    printf("{\"screenshot\":{\"w\":%lu,\"h\":%lu,\"format\":\"rgb565le\"}}\n", (unsigned long)w, (unsigned long)h);
    // One base64 line per pixel row, prefixed so a reader can resync.
    unsigned char out[1024];
    for (uint32_t row = 0; row < h; row++) {
        size_t n = 0;
        mbedtls_base64_encode(out, sizeof(out), &n, buf->data + row * stride, w * 2);
        out[n] = 0;
        printf("R%03lu %s\n", (unsigned long)row, out);
        if (row % 16 == 15) fflush(stdout);
    }
    printf("{\"screenshot_end\":true}\n");
    lv_draw_buf_destroy(buf);
}

}
