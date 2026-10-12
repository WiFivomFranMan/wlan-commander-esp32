// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcd_display.h"
#include "lcd_raster.h"
#include "spectrum_view.h"
#include "generator_view.h"
#include "hal/usb_serial_jtag_ll.h"
#include <atomic>
#include <new>
#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

namespace {
#if WLC_BOARD_1_69
// Waveshare ESP32-C5-Touch-LCD-1.69: ST7789V2 240x280 (20-row panel gap), driven as 280x240
// landscape. Backlight is GPIO26 through a transistor; no co-processor, no microSD.
constexpr unsigned WIDTH = 280, HEIGHT = 240, STRIP = 4, PANEL_GAP = 20;
constexpr gpio_num_t LCD_DC = GPIO_NUM_24, LCD_CS = GPIO_NUM_27, LCD_MOSI = GPIO_NUM_7, LCD_SCLK = GPIO_NUM_23;
constexpr gpio_num_t LCD_BACKLIGHT = GPIO_NUM_26, I2C_SDA = GPIO_NUM_8, I2C_SCL = GPIO_NUM_9;
#else
// Waveshare ESP32-C5-Touch-LCD-2.8: ST7789 240x320, driven here as 320x240 landscape. The panel's
// reset line and backlight PWM belong to the CH32V003 co-processor at I2C 0x24, not to GPIOs.
constexpr unsigned WIDTH = 320, HEIGHT = 240, STRIP = 4, PANEL_GAP = 0;
constexpr gpio_num_t LCD_DC = GPIO_NUM_9, LCD_CS = GPIO_NUM_10, LCD_MOSI = GPIO_NUM_7, LCD_SCLK = GPIO_NUM_6;
constexpr gpio_num_t SD_CS = GPIO_NUM_23, I2C_SDA = GPIO_NUM_0, I2C_SCL = GPIO_NUM_1;
constexpr uint8_t CH32_ADDR = 0x24, CH32_DIR = 0x02, CH32_OUT = 0x03, CH32_PWM = 0x05;
constexpr uint8_t CH32_TOUCH_RST = 1 << 0, CH32_LCD_RST = 1 << 1, CH32_AMP = 1 << 3;
#endif
// The status model and spectrum view are laid out 320 wide; narrower panels sample them.
constexpr unsigned MODEL_WIDTH = 320;
#if WLC_BOARD_1_69
#define WLC_SENSOR_NAME "wlc-sensor-1.69"
#else
#define WLC_SENSOR_NAME "wlc-sensor-2.8"
#endif
constexpr uint8_t BACKLIGHT_LEVEL = 76;   // ~30%, as on the PoC board
struct Display {
    spi_device_handle_t spi = nullptr;
    uint16_t* pixels = nullptr;
    LcdModel model;
    LcdFrame frame, previous;
    uint32_t sampled_at = 0, drawn_at = 0;
    uint16_t dirty = 0x1ff;
    unsigned y = HEIGHT;
    bool sampled = false, painted = false, bus_ready = false;
    i2c_master_bus_handle_t i2c = nullptr;
    SpectrumView* spectrum = nullptr;
    GeneratorView* generator = nullptr;
    i2c_master_dev_handle_t touch = nullptr;
    bool touch_ok = false;
    wlc::generator::Press press;
    uint32_t touch_at = 0, generator_at = 0;
    wlc::generator::Action virtual_action = wlc::generator::Action::None;
    i2c_master_dev_handle_t ch32 = nullptr;
    bool backlight_ready = false;
    uint8_t led_number = 0;
};
Display* display;
std::atomic<uint16_t> pending_identify{0};
std::atomic<bool> ready{false};
std::atomic<uint8_t> visible_number{0};
std::atomic<uint32_t> visible_until{0};

bool transfer(bool data, const void* bytes, size_t size) {
    if (!size) return true;
    gpio_set_level(LCD_DC, data);
    spi_transaction_t t{}; t.length = size * 8;
    if (size <= 4) { t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, bytes, size); }
    else t.tx_buffer = bytes;
    return spi_device_polling_transmit(display->spi, &t) == ESP_OK;
}
#if !WLC_BOARD_1_69
bool ch32_write(uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(display->ch32, data, 2, 100) == ESP_OK;
}
#endif
bool command(uint8_t cmd, const void* data = nullptr, size_t size = 0) {
    return transfer(false, &cmd, 1) && transfer(true, data, size);
}
void release_display() {
    ready.store(false); visible_number.store(0); pending_identify.store(0);
    if (!display) return;
#if WLC_BOARD_1_69
    if (display->backlight_ready) ledcDetach(LCD_BACKLIGHT);
    gpio_set_level(LCD_BACKLIGHT, 0);
#else
    if (display->ch32) { ch32_write(CH32_PWM, 0); i2c_master_bus_rm_device(display->ch32); }
#endif
    if (display->touch) i2c_master_bus_rm_device(display->touch);
    if (display->i2c) i2c_del_master_bus(display->i2c);
    heap_caps_free(display->generator);
    if (display->spi) spi_bus_remove_device(display->spi);
    if (display->bus_ready) spi_bus_free(SPI2_HOST);
    heap_caps_free(display->pixels);
    display->~Display(); heap_caps_free(display); display = nullptr;
}
bool paint_strip(unsigned y, unsigned height) {
    for (unsigned line = 0; line < height; ++line) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned mx = x * MODEL_WIDTH / WIDTH;
            uint16_t color = display->generator ? display->generator->pixel(mx, y + line) :
                             display->spectrum ? display->spectrum->pixel(mx, y + line)
                                               : lcd_pixel(display->frame, mx, y + line);
            display->pixels[line * WIDTH + x] = (color << 8) | (color >> 8);
        }
    }
    uint16_t top = y, bottom = top + height - 1;
    uint16_t left = PANEL_GAP, right = PANEL_GAP + WIDTH - 1;   // landscape: the panel gap is on X
    uint8_t columns[4] = {(uint8_t)(left >> 8),(uint8_t)left,(uint8_t)(right >> 8),(uint8_t)right};
    uint8_t rows[4] = {(uint8_t)(top >> 8),(uint8_t)top,(uint8_t)(bottom >> 8),(uint8_t)bottom};
    return command(0x2a, columns, 4) && command(0x2b, rows, 4) &&
           command(0x2c, display->pixels, WIDTH * height * 2);
}
}

bool wlc_lcd_init() {
    gpio_config_t pins{};
#if WLC_BOARD_1_69
    pins.pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_BACKLIGHT);
    pins.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&pins) != ESP_OK) return false;
    gpio_set_level(LCD_BACKLIGHT, 0);
#else
    pins.pin_bit_mask = (1ULL << LCD_DC) | (1ULL << SD_CS);
    pins.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&pins) != ESP_OK) return false;
    gpio_set_level(SD_CS, 1); // Keep the unused microSD deselected on the shared SPI bus.
#endif
    void* memory = heap_caps_malloc(sizeof(Display), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return false;
    display = new(memory) Display;
    display->pixels = (uint16_t*)heap_caps_malloc(WIDTH * STRIP * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!display->pixels) { release_display(); return false; }
#if !WLC_BOARD_1_69
    i2c_master_bus_config_t i2c_bus{};
    i2c_bus.i2c_port = I2C_NUM_0; i2c_bus.sda_io_num = I2C_SDA; i2c_bus.scl_io_num = I2C_SCL;
    i2c_bus.clk_source = I2C_CLK_SRC_DEFAULT; i2c_bus.glitch_ignore_cnt = 7;
    i2c_bus.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&i2c_bus, &display->i2c) != ESP_OK) { release_display(); return false; }
    i2c_device_config_t ch32{};
    ch32.dev_addr_length = I2C_ADDR_BIT_LEN_7; ch32.device_address = CH32_ADDR; ch32.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(display->i2c, &ch32, &display->ch32) != ESP_OK) { release_display(); return false; }
#endif
    spi_bus_config_t bus{};
    bus.mosi_io_num = LCD_MOSI; bus.miso_io_num = -1; bus.sclk_io_num = LCD_SCLK;
    bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = WIDTH * STRIP * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { release_display(); return false; }
    display->bus_ready = true;
    spi_device_interface_config_t device{};
    device.clock_speed_hz = 40000000; device.mode = 0;
    device.spics_io_num = LCD_CS; device.queue_size = 1;
    if (spi_bus_add_device(SPI2_HOST, &device, &display->spi) != ESP_OK) { release_display(); return false; }
#if WLC_BOARD_1_69
    bool ok = true;   // LCD reset is not wired to a controllable line; 0x01 below resets it
#else
    // Same sequence the Waveshare BSP uses: all expander pins outputs, then pulse LCD reset.
    const uint8_t idle = CH32_TOUCH_RST | CH32_LCD_RST | CH32_AMP;
    bool ok = ch32_write(CH32_DIR, 0xff) && ch32_write(CH32_OUT, idle & ~CH32_LCD_RST);
    delay(50);
    ok = ok && ch32_write(CH32_OUT, idle);
    delay(120);
#endif
    ok = ok && command(0x01); delay(120);
    ok = ok && command(0x11); delay(120);
    uint8_t format = 0x55, orientation = 0x60;    // MV|MX: landscape, USB connector on the right
    ok = ok && command(0x3a, &format, 1) && command(0x36, &orientation, 1) &&
         command(0x21) && command(0x13);
    // Clear the whole panel once: the model only draws its nine text rows, and the launcher
    // image's last frame would otherwise stay visible below them.
    for (unsigned y = 0; ok && y < HEIGHT; y += STRIP) ok = paint_strip(y, STRIP);
#if WLC_BOARD_1_69
    ok = ok && command(0x29);
    display->backlight_ready = ok && ledcAttach(LCD_BACKLIGHT, 5000, 8) && ledcWrite(LCD_BACKLIGHT, BACKLIGHT_LEVEL);
    ok = ok && display->backlight_ready;
#else
    ok = ok && command(0x29) && ch32_write(CH32_PWM, BACKLIGHT_LEVEL);
#endif
    if (!ok) { release_display(); return false; }
    ready.store(true);
    return true;
}

bool wlc_lcd_identify(unsigned number, unsigned seconds) {
    if (!ready.load() || !LcdModel::valid_identify(number, seconds)) return false;
    // BLE callbacks only stage a bounded command. Main loop owns the display/model.
    pending_identify.store((seconds << 8) | number);
    return true;
}

void wlc_lcd_show_mode(LcdSnapshot::Mode mode) {
    if (!ready.load()) return;
    // Finish this one transition before acquisition/TX deadlines are armed.
    // During RF the screen makes its frozen state explicit, rather than saying idle.
    LcdSnapshot s{}; s.mode = mode; s.now = millis(); esp_read_mac(s.mac, ESP_MAC_WIFI_STA);
    display->model.identify_number = 0;
    pending_identify.store(0); visible_number.store(0);
    wlc_lcd_led_override(s.now);
    display->frame = display->model.frame(s);
    snprintf(display->frame.rows[2], 27, "DISPLAY PAUSED DURING RF");
    for (unsigned row = 3; row < 9; ++row) display->frame.rows[row][0] = 0;
    display->frame.finish();
    for (unsigned y = 0; y < HEIGHT; y += STRIP) {
        unsigned height = y + STRIP <= HEIGHT ? STRIP : HEIGHT - y;
        if (!paint_strip(y, height)) { release_display(); return; }
    }
    display->y = HEIGHT; display->painted = false;
}

void wlc_lcd_tick(const LcdSnapshot& snapshot) {
    if (!ready.load()) return;
    uint16_t request = pending_identify.exchange(0);
    if (request) {
        display->model.identify(request & 255, request >> 8, snapshot.now);
        visible_number.store(request & 255);
        visible_until.store(display->model.identify_until);
        display->y = HEIGHT; display->painted = false;
    }
    uint8_t number = display->model.active_identify(snapshot.now);
    visible_number.store(number);
    if (!number && display->frame.identify) { display->y = HEIGHT; display->painted = false; }
    if (!display->sampled || (uint32_t)(snapshot.now - display->sampled_at) >= 500) {
        display->model.sample(snapshot);
        display->sampled_at = snapshot.now; display->sampled = true;
    }
    if (display->y >= HEIGHT) {
        if (display->painted && (uint32_t)(snapshot.now - display->drawn_at) < 500) return;
        display->previous = display->frame;
        display->frame = display->model.frame(snapshot);
        display->dirty = 0;
        bool all = !display->painted || display->frame.identify != display->previous.identify;
        for (unsigned row = 0; row < 9; ++row) {
            if (all || strcmp(display->frame.rows[row], display->previous.rows[row]) ||
                (row == 1 && display->frame.accent != display->previous.accent)) display->dirty |= 1 << row;
        }
        display->y = 0; display->drawn_at = snapshot.now;
    }
    // At most one four-pixel strip per main-loop pass; radio work runs first.
    while (display->y < HEIGHT) {
        unsigned y = display->y, height = y + STRIP <= HEIGHT ? STRIP : HEIGHT - y;
        display->y += height;
        unsigned mask = (1 << (y / 19)) | (1 << ((y + height - 1) / 19));
        if (!(mask & display->dirty)) continue;
        if (!paint_strip(y, height)) release_display();
        break;
    }
    if (display && display->y >= HEIGHT) display->painted = true;
}

bool wlc_lcd_led_override(uint32_t now) {
    if (!ready.load()) return false;
    uint8_t number = visible_number.load();
    if (number && (int32_t)(now - visible_until.load()) >= 0) number = 0;
    if (display->led_number != number) {
        // No addressable LED on this board (GPIO8 is the microSD MISO line); the screen alone
        // shows the Identify number.
        display->led_number = number;
    }
    return number != 0;
}

void wlc_lcd_info(char* output, size_t capacity) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t now = millis(), until = visible_until.load();
    uint8_t number = visible_number.load();
    if (number && (int32_t)(now - until) >= 0) number = 0;
    snprintf(output, capacity,
        "{\"version\":1,\"firmware\":\"" WLC_SENSOR_NAME "\",\"board\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"display\":%s,\"identify\":true,\"identify_number\":%u,\"identify_remaining_ms\":%lu}",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],ready.load() ? "true" : "false", number,
        (unsigned long)(number ? until - now : 0));
}

bool wlc_lcd_spectrum_begin() {
    if (!ready.load()) return false;
    if (display->spectrum) return true;
    void* memory = heap_caps_calloc(1, sizeof(SpectrumView), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return false;
    display->spectrum = new(memory) SpectrumView;
    display->spectrum->reset();
    // The waterfall is optional: BLE is down in spectrum mode, so this normally fits.
    display->spectrum->fall = (uint8_t*)heap_caps_calloc(SpectrumView::FALL_ROWS * SpectrumView::FALL_COLS, 1,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return true;
}

void wlc_lcd_spectrum_ranges(const uint16_t (*ranges)[2], unsigned count) {
    if (ready.load() && display->spectrum) display->spectrum->set_ranges(ranges, count);
}

void wlc_lcd_spectrum_frame(const uint8_t* db, unsigned bins, unsigned center_mhz) {
    if (ready.load() && display->spectrum) display->spectrum->push_capture(db, bins, center_mhz);
}

void wlc_lcd_spectrum_commit() {
    if (ready.load() && display->spectrum) display->spectrum->commit();
}



void wlc_lcd_spectrum_draw(const char* title, const char* axis) {
    if (!ready.load() || !display->spectrum) return;
    snprintf(display->spectrum->title, sizeof(display->spectrum->title), "%s", title);
    snprintf(display->spectrum->axis, sizeof(display->spectrum->axis), "%s", axis);
    // Called between acquisitions only: never any SPI traffic while the SDR is capturing.
    for (unsigned y = 0; y < HEIGHT; y += STRIP)
        if (!paint_strip(y, STRIP)) { release_display(); return; }
}

void wlc_lcd_spectrum_end() {
    if (!display || !display->spectrum) return;
    heap_caps_free(display->spectrum->fall);
    display->spectrum->~SpectrumView();
    heap_caps_free(display->spectrum);
    display->spectrum = nullptr;
    display->y = HEIGHT; display->painted = false;   // repaint the status page in full
}

namespace {
constexpr int TOUCH_TIMEOUT = 20;
bool touch_read(bool &pressed, int &x, int &y, bool &fresh) {
    if (!display || !display->touch) return false;
    uint8_t b[9]{};
#if WLC_BOARD_1_69
    uint8_t reg = 0x02;
    if (i2c_master_transmit_receive(display->touch, &reg, 1, b, 5, TOUCH_TIMEOUT) != ESP_OK) return false;
    pressed = (b[0] & 0x0f) == 1; fresh = (b[1] >> 6) == 0;
    uint16_t px = ((b[1] & 15) << 8) | b[2], py = ((b[3] & 15) << 8) | b[4];
#else
    uint8_t reg[] = {0xd0,0x07,0,0}, ack[] = {0xd0,0,2,0xab};
    if (i2c_master_transmit(display->touch, reg, 4, TOUCH_TIMEOUT) != ESP_OK ||
        i2c_master_receive(display->touch, b, 9, TOUCH_TIMEOUT) != ESP_OK ||
        i2c_master_transmit(display->touch, ack, 4, TOUCH_TIMEOUT) != ESP_OK) return false;
    pressed = (b[3] & 15) == 1 && (b[8] & 0xf0) != 0; fresh = (b[8] >> 4) == 1;
    uint16_t px = ((b[7] & 15) << 8) | b[4], py = ((b[7] & 0xf0) << 4) | b[5];
#endif
    if (pressed && !wlc::generator::landscape(px, py, WIDTH, x, y)) pressed = false;
    return true;
}
struct Shot {
    GeneratorView view;
    char output[900];
    unsigned length = 0, offset = 0, row = 0;
    uint32_t progress = 0;
};
Shot *shot;
bool shot_aborted;
}

bool wlc_lcd_generator_begin() {
    if (!ready.load()) return false;
    display->generator = (GeneratorView *)heap_caps_calloc(1, sizeof(GeneratorView), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!display->generator) return false;
#if WLC_BOARD_1_69
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0; bus.sda_io_num = I2C_SDA; bus.scl_io_num = I2C_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT; bus.glitch_ignore_cnt = 7; bus.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus, &display->i2c) != ESP_OK) return false;
    // TCA9554 EXIO1 is the touch reset; preserve every other output/direction.
    i2c_device_config_t expcfg{};
    expcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7; expcfg.device_address = 0x20; expcfg.scl_speed_hz = 400000;
    i2c_master_dev_handle_t exp;
    if (i2c_master_bus_add_device(display->i2c, &expcfg, &exp) != ESP_OK) return false;
    uint8_t reg = 3, dir = 0, out = 0;
    bool ok = i2c_master_transmit_receive(exp, &reg, 1, &dir, 1, TOUCH_TIMEOUT) == ESP_OK;
    reg = 1;
    ok = ok && i2c_master_transmit_receive(exp, &reg, 1, &out, 1, TOUCH_TIMEOUT) == ESP_OK;
    uint8_t bytes[] = {3, (uint8_t)(dir & ~2)};
    ok = ok && i2c_master_transmit(exp, bytes, 2, TOUCH_TIMEOUT) == ESP_OK;
    bytes[0] = 1; bytes[1] = out & ~2;
    ok = ok && i2c_master_transmit(exp, bytes, 2, TOUCH_TIMEOUT) == ESP_OK;
    delay(10); bytes[1] = out | 2;
    ok = ok && i2c_master_transmit(exp, bytes, 2, TOUCH_TIMEOUT) == ESP_OK;
    i2c_master_bus_rm_device(exp); delay(100);
    if (!ok) return false;
    constexpr uint8_t address = 0x15;
#else
    const uint8_t idle = CH32_TOUCH_RST | CH32_LCD_RST | CH32_AMP;
    if (!ch32_write(CH32_OUT, idle & ~CH32_TOUCH_RST)) return false;
    delay(10);
    if (!ch32_write(CH32_OUT, idle)) return false;
    delay(100);
    constexpr uint8_t address = 0x58;
#endif
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7; cfg.device_address = address; cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(display->i2c, &cfg, &display->touch) != ESP_OK) return false;
    bool pressed = false, fresh = false; int x = 0, y = 0;
    display->touch_ok = touch_read(pressed, x, y, fresh);
    display->press.update(pressed, x, y, WIDTH, fresh);
    return display->touch_ok;
}
bool wlc_lcd_generator_healthy() { return ready.load() && display->generator && display->touch_ok; }
wlc::generator::Action wlc_lcd_generator_input() {
    using namespace wlc::generator;
    if (!wlc_lcd_generator_healthy()) return Action::None;
    uint32_t now = millis();
    if ((uint32_t)(now - display->touch_at) < 20) return Action::None;
    display->touch_at = now;
    bool pressed = false, fresh = false; int x = 0, y = 0;
    if (!touch_read(pressed, x, y, fresh)) { display->touch_ok = false; return Action::None; }
    Action physical = display->press.update(pressed, x, y, WIDTH, fresh);
    Action remote = display->virtual_action; display->virtual_action = Action::None;
    return physical == Action::None ? remote : physical;
}
bool wlc_lcd_generator_tap(int x, int y) {
    if (!wlc_lcd_generator_healthy()) return false;
    // Injection is a complete press+release; it cannot bypass a held physical finger.
    if (display->press.down || !display->press.released) return false;
    auto action = wlc::generator::hit(x, y, WIDTH);
    if (action == wlc::generator::Action::None || display->virtual_action != wlc::generator::Action::None) return false;
    display->virtual_action = action; return true;
}
void wlc_lcd_generator_cancel_input() {
    if (!display || !display->generator) return;
    display->virtual_action = wlc::generator::Action::None;
    // Stop wins over an injected Start waiting for the next touch poll and
    // over a finger held down before Stop. Observe release before a new press.
    display->press = wlc::generator::Press{};
}
void wlc_lcd_generator_tick(const wlc::generator::Run &run, uint32_t session_ms) {
    if (!ready.load() || !display->generator) return;
    uint32_t now = millis();
    if (display->y >= HEIGHT) {
        if ((uint32_t)(now - display->generator_at) < 200) return;
        display->generator->update(run, now, display->touch_ok, session_ms);
        display->generator_at = now; display->y = 0;
    }
    unsigned y = display->y; display->y += STRIP;
    if (!paint_strip(y, STRIP)) release_display();
}
bool wlc_lcd_generator_shot() {
    if (!ready.load() || !display->generator || shot) return false;
    shot = (Shot *)heap_caps_calloc(1, sizeof(Shot), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!shot) return false;
    shot->view = *display->generator;
    shot->length = snprintf(shot->output, sizeof(shot->output), "{\"screenshot\":{\"w\":%u,\"h\":%u,\"format\":\"rgb565le\"}}\n", WIDTH, HEIGHT);
    shot->progress = millis(); return true;
}
bool wlc_lcd_generator_shot_cancel() {
    if (!shot && !shot_aborted) return false;
    if (shot) heap_caps_free(shot);
    shot = nullptr; shot_aborted = false; return true;
}
void wlc_lcd_generator_shot_tick() {
    if (!shot) return;
    if ((uint32_t)(millis() - shot->progress) >= 3000) {
        heap_caps_free(shot); shot = nullptr; shot_aborted = true;
        // The next command emits a newline before its reply even if this
        // truncated row could not be completed while the host was absent.
        return;
    }
    if (!usb_serial_jtag_ll_txfifo_writable()) return;
    unsigned left = shot->length - shot->offset;
    if (left) {
        unsigned n = usb_serial_jtag_ll_write_txfifo((uint8_t *)shot->output + shot->offset, left > 64 ? 64 : left);
        usb_serial_jtag_ll_txfifo_flush(); shot->offset += n;
        if (n) shot->progress = millis();
        return;
    }
    if (shot->row > HEIGHT) { heap_caps_free(shot); shot = nullptr; return; }
    shot->offset = 0;
    if (shot->row == HEIGHT) {
        shot->length = snprintf(shot->output, sizeof(shot->output), "{\"screenshot_end\":true}\n"); ++shot->row; return;
    }
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned n = snprintf(shot->output, sizeof(shot->output), "R%03u ", shot->row);
    // Encode RGB565LE directly, without a framebuffer or blocking transport.
    for (unsigned byte = 0; byte < WIDTH * 2; byte += 3) {
        uint32_t triple = 0; unsigned count = WIDTH * 2 - byte; if (count > 3) count = 3;
        for (unsigned j = 0; j < count; ++j) {
            unsigned b = byte + j;
            uint16_t pixel = shot->view.pixel((b / 2) * MODEL_WIDTH / WIDTH, shot->row);
            triple |= (uint32_t)((b & 1) ? pixel >> 8 : pixel & 255) << (16 - j * 8);
        }
        shot->output[n++] = alphabet[(triple >> 18) & 63]; shot->output[n++] = alphabet[(triple >> 12) & 63];
        shot->output[n++] = count > 1 ? alphabet[(triple >> 6) & 63] : '=';
        shot->output[n++] = count > 2 ? alphabet[triple & 63] : '=';
    }
    shot->output[n++] = '\n'; shot->length = n; ++shot->row;
}
