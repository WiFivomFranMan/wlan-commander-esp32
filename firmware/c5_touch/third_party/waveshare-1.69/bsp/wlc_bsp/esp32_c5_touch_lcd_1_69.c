/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_spiffs.h"

// #include "esp_lcd_st7789.h"
#include "esp_lcd_touch_cst816s.h"

#include "bsp/esp32_c5_touch_lcd_1_69.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"
#include "bsp/config.h"
#include "bsp/display.h"
#include "esp_codec_dev_defaults.h"

#include "lvgl.h"
// #include "esp_lvgl_port.h"
#include "esp_lv_adapter.h"

static const char *TAG = "ESP32-C5-TOUCH-LCD-1.69";

/**
 * @brief I2C handle for BSP usage
 *
 * In IDF v5.4 you can call i2c_master_get_bus_handle(BSP_I2C_NUM, i2c_master_bus_handle_t *ret_handle)
 * from #include "esp_private/i2c_platform.h" to get this handle
 *
 * For IDF 5.2 and 5.3 you must call bsp_i2c_get_handle()
 */
static i2c_bus_handle_t i2c_bus = NULL;
static i2c_master_bus_handle_t i2c_handle = NULL;
static bool i2c_initialized = false;

static bq27220_handle_t bq27220 = NULL;
static i2c_bus_device_handle_t bq27220_i2c_dev = NULL;
static bool bq27220_initialized = false;
static uint16_t bq27220_capacity_mah = 0;

static esp_io_expander_handle_t io_expander = NULL; // IO expander tca9554 handle

temperature_sensor_handle_t temp_sensor = NULL;
static pcf85063a_dev_t pcf85063a_dev;

qmi8658_dev_t qmi8658_dev;

static bool spi_initialized = false;

static lv_display_t *disp;
static lv_indev_t *disp_indev = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL; // LCD panel handle
static esp_lcd_panel_io_handle_t io_handle = NULL;
static bsp_display_partial_mode_t bsp_display_partial_mode = {
    .enabled = false,
    .buffer_height = BSP_DISPLAY_PARTIAL_BUFFER_HEIGHT_DEFAULT,
};

uint8_t brightness;

typedef struct {
    uint16_t voltage;
    int16_t current;
    uint16_t remaining_capacity;
    uint16_t full_charge_capacity;
    uint16_t temperature;
    uint16_t cycle_count;
    uint16_t state_of_charge;
    int16_t average_power;
    int16_t maxload_current;
    uint16_t time_to_empty;
    uint16_t time_to_full;
    uint16_t state_of_health;
} bsp_bq27220_measurement_t;

static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL; /* Codec data interface */
static const audio_codec_ctrl_if_t *audio_ctrl_if = NULL;
static const audio_codec_gpio_if_t *audio_gpio_if = NULL;
static const audio_codec_if_t *audio_codec_if = NULL;
static esp_codec_dev_handle_t audio_codec_dev = NULL;

#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static i2s_std_config_t bsp_i2s_duplex_stereo_cfg(uint32_t sample_rate)
{
    i2s_std_config_t cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    return cfg;
}

/**************************************************************************************************
 *
 * I2C Function
 *
 **************************************************************************************************/
esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized)
    {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400 * 1000,
    };
    i2c_bus = i2c_bus_create(BSP_I2C_NUM, &conf);

    i2c_handle = i2c_bus_get_internal_bus_handle(i2c_bus);

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_bus_delete(i2c_bus));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

/**************************************************************************************************
 *
 * SPI Function
 *
 **************************************************************************************************/
esp_err_t bsp_spi_init(void)
{
    /* SPI was initialized before */
    if (spi_initialized) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BSP_LCD_DATA,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = BSP_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8, //取两者数据量最多的
    };

    BSP_ERROR_CHECK_RETURN_ERR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus_cfg, SPI_DMA_CH_AUTO));
    spi_initialized = true;
    return ESP_OK;
}

/**************************************************************************************************
 *
 * SPIFFS Function
 *
 **************************************************************************************************/
esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

/**********************************************************************************************************
 *
 * Audio Function
 *
 **********************************************************************************************************/
esp_err_t bsp_audio_poweramp_enable(bool enable)
{
    BSP_ERROR_CHECK_RETURN_ERR(esp_io_expander_set_level(io_expander, BSP_POWER_AMP_IO, (uint8_t)enable));
    return ESP_OK;
}

static esp_codec_dev_handle_t bsp_audio_codec_duplex_init(void)
{
    if (audio_codec_dev != NULL) {
        return audio_codec_dev;
    }

    if (i2s_data_if == NULL) {
        ESP_ERROR_CHECK(bsp_i2c_init());
        ESP_ERROR_CHECK(bsp_audio_init(NULL));
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    audio_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(audio_ctrl_if);

    audio_gpio_if = audio_codec_new_gpio();
    assert(audio_gpio_if);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = audio_ctrl_if,
        .gpio_if = audio_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
        .no_dac_ref = false,
    };
    audio_codec_if = es8311_codec_new(&es8311_cfg);
    assert(audio_codec_if);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = audio_codec_if,
        .data_if = i2s_data_if,
    };

    BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_poweramp_enable(true));
    audio_codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    assert(audio_codec_dev);
    return audio_codec_dev;
}

 esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = bsp_i2s_duplex_stereo_cfg(BSP_AUDIO_OUTPUT_SAMPLE_RATE_HZ);
    const i2s_std_config_t *std_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        std_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, std_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    }

    if (i2s_rx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, std_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .tx_handle = i2s_tx_chan,
        .rx_handle = i2s_rx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_FAIL);

    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return bsp_audio_codec_duplex_init();
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    return bsp_audio_codec_duplex_init();
}


/**********************************************************************************************************
 *
 * Display Function
 *
 **********************************************************************************************************/

esp_err_t bsp_display_new(const bsp_display_cfg_t *cfg, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;

    if (!spi_initialized) {
        ESP_LOGI(TAG, "Initialize SPI bus");
        bsp_spi_init();
    }
    
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = GPIO_NUM_27,
        .pclk_hz = BSP_LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(BSP_LCD_SPI_NUM,
                                             &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .flags = {.reset_active_high = 0}
    };

    ESP_LOGI(TAG, "Install ST7789 panel driver");

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    esp_lcd_panel_disp_on_off(panel_handle, true);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0x14));

    esp_lcd_panel_mirror(panel_handle, cfg->touch_flags.mirror_x, cfg->touch_flags.mirror_y);
    esp_lcd_panel_swap_xy(panel_handle, cfg->touch_flags.swap_xy);

    if (ret_panel)
    {
        *ret_panel = panel_handle;
    }
    if (ret_io)
    {
        *ret_io = io_handle;
    }
    return ret;
}

esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    BSP_NULL_CHECK(bsp_io_expander_init(), ESP_FAIL);
    esp_io_expander_set_dir(io_expander, BSP_LCD_TOUCH_RST, IO_EXPANDER_OUTPUT);
    //RESET LCD
    esp_io_expander_set_level(io_expander, BSP_LCD_TOUCH_RST, 0);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    esp_io_expander_set_level(io_expander, BSP_LCD_TOUCH_RST, 1);
    vTaskDelay(200 / portTICK_PERIOD_MS);

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = -1, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = cfg->touch_flags.swap_xy,
            .mirror_x = cfg->touch_flags.mirror_x,
            .mirror_y = cfg->touch_flags.mirror_y,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    return esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
     BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(cfg, &panel_handle, &io_handle));

    ESP_LOGI(TAG, "Registering RGB display");
    esp_lv_adapter_display_config_t display_config =  ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
        panel_handle,           	// LCD 面板句柄
        io_handle,        			// LCD 面板 IO 句柄（某些接口可为 NULL）
        BSP_LCD_H_RES,             		// 水平分辨率
        BSP_LCD_V_RES,             	// 垂直分辨率
        cfg->rotation 	// 旋转角度
    );
    display_config.tear_avoid_mode = cfg->tear_avoid_mode;

    if (bsp_display_partial_mode.enabled) {
        display_config.profile.buffer_height = bsp_display_partial_mode.buffer_height;
        display_config.profile.require_double_buffer = false;
    }
    
    return esp_lv_adapter_register_display(&display_config);
}

esp_err_t bsp_display_set_partial_mode(bool enable, uint16_t buffer_height)
{
    if (disp != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (enable && (buffer_height > BSP_LCD_V_RES)) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_display_partial_mode.enabled = enable;
    bsp_display_partial_mode.buffer_height = BSP_DISPLAY_PARTIAL_BUFFER_HEIGHT_DEFAULT;

    if (enable && (buffer_height != 0)) {
        bsp_display_partial_mode.buffer_height = buffer_height;
    }

    return ESP_OK;
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(cfg, &tp));
    assert(tp);

   /* Add touch input (for selected screen) */
    const esp_lv_adapter_touch_config_t touch_cfg = {
        .disp = disp,
        .handle = tp,
        .scale = {.x = 1.0f, .y = 1.0f},
    };

    return esp_lv_adapter_register_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0}};

    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(esp_lv_adapter_init(&cfg->lv_adapter_cfg));
        
    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(cfg, disp), NULL);

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

#define LCD_LEDC_CH            CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

esp_err_t bsp_display_brightness_init(void)
{
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&LCD_backlight_channel));

    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    } else if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    int flipped_brightness = brightness_percent;

    brightness = (uint8_t)flipped_brightness;

    ESP_LOGI(TAG, "Setting flipped LCD backlight: %d%% (original: %d%%)", flipped_brightness, brightness_percent);

    uint32_t duty_cycle = (1023 * flipped_brightness) / 100;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));

    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return -1;
    }

    return brightness * 100 / 255;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

void bsp_display_rotate(lv_display_t *disp, lv_display_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}
/**************************************************************************************************
 *
 * IO Expander Function
 *
 **************************************************************************************************/
esp_io_expander_handle_t bsp_io_expander_init(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    if (!io_expander) {
        BSP_ERROR_CHECK_RETURN_NULL(esp_io_expander_new_i2c_tca9554(i2c_handle, BSP_IO_EXPANDER_I2C_ADDRESS, &io_expander));
        esp_io_expander_set_dir(io_expander, BSP_LCD_TOUCH_RST, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, BSP_LCD_TOUCH_RST, 1);
        esp_io_expander_set_dir(io_expander, BSP_POWER_AMP_IO, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, BSP_POWER_AMP_IO, 1);
    }
    return io_expander;
}

esp_err_t bsp_io_expander_set_dir(uint8_t pin, uint8_t dir)
{
    uint32_t pin_mask = (1UL << pin);

    return esp_io_expander_set_dir(io_expander, pin_mask, dir);
}

esp_err_t bsp_io_expander_set_level(uint8_t pin, uint8_t level)
{
    uint32_t pin_mask = (1UL << pin);

    return esp_io_expander_set_level(io_expander, pin_mask, level);
}

esp_err_t bsp_io_expander_get_level(uint8_t pin, uint8_t *level)
{
    esp_err_t ret;
    uint32_t level_mask = 0;

    // 1. pin → mask
    uint32_t pin_mask = (1 << pin);

    // 2. 调用底层接口
    ret = esp_io_expander_get_level(io_expander, pin_mask, &level_mask);
    if (ret != ESP_OK) {
        return ret;
    }

    // 3. 从mask取出该pin电平
    *level = (level_mask & pin_mask) ? 1 : 0;

    return ESP_OK;
}

/**************************************************************************************************
 *
 * ESP32 CPU Temperature Function
 *
 **************************************************************************************************/
esp_err_t bsp_cpu_temp_init()
{
    ESP_LOGI(TAG, "Install temperature sensor, expected temp ranger range: 10~50 ℃");
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
    
    ESP_LOGI(TAG, "Enable temperature sensor");
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));

    return ESP_OK;
}

esp_err_t bsp_get_cpu_temp(float *tsens_value)
{
    ESP_LOGI(TAG, "Read temperature");
    ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, tsens_value));
    ESP_LOGI(TAG, "Temperature value %.02f ℃", *tsens_value);

    return ESP_OK;
}

/**************************************************************************************************
 *
 * PCF85063A RTC Function
 *
 **************************************************************************************************/
esp_err_t bsp_rtc_init()
{
    if (!i2c_initialized)
    {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    }
    esp_err_t ret = pcf85063a_init(&pcf85063a_dev, i2c_handle, BSP_RTC_I2C_ADDRESS);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Override the component default CAP_SEL to 7 pF for this board. */
    uint8_t ctrl1_buf[2] = {PCF85063A_RTC_CTRL_1_ADDR, PCF85063A_RTC_CTRL_1_DEFAULT};
    ret = pcf85063a_write_register(&pcf85063a_dev, ctrl1_buf, 2);
    return ret;
}

esp_err_t bsp_set_rtc_time_date(pcf85063a_datetime_t time)
{
    uint8_t ctrl1 = 0;
    esp_err_t ret = pcf85063a_read_register(&pcf85063a_dev, PCF85063A_RTC_CTRL_1_ADDR, &ctrl1, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t ctrl1_buf[2] = {
        PCF85063A_RTC_CTRL_1_ADDR,
        (uint8_t)(ctrl1 | PCF85063A_RTC_CTRL_1_STOP)
    };
    ret = pcf85063a_write_register(&pcf85063a_dev, ctrl1_buf, 2);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = pcf85063a_set_time_date(&pcf85063a_dev, time);
    if (ret != ESP_OK) {
        return ret;
    }

    ctrl1_buf[1] = (uint8_t)(ctrl1 & (uint8_t)(~PCF85063A_RTC_CTRL_1_STOP));
    ret = pcf85063a_write_register(&pcf85063a_dev, ctrl1_buf, 2);
    return ret;
}

esp_err_t bsp_set_rtc_alarm_time(pcf85063a_datetime_t time)
{
    esp_err_t ret = pcf85063a_set_alarm(&pcf85063a_dev, time);
    return ret;
}

esp_err_t bsp_get_rtc_time_date(pcf85063a_datetime_t *time)
{
    esp_err_t ret = pcf85063a_get_time_date(&pcf85063a_dev, time);
    return ret;
}

esp_err_t bsp_get_rtc_alarm_time(pcf85063a_datetime_t *time)
{
    esp_err_t ret = pcf85063a_get_alarm(&pcf85063a_dev, time);
    return ret;
}

esp_err_t bsp_enable_rtc_alarm()
{
    esp_err_t ret = pcf85063a_enable_alarm(&pcf85063a_dev);
    return ret;
}

esp_err_t bsp_datetime_to_str(char *datetime_str, size_t size, pcf85063a_datetime_t time)
{
    pcf85063a_datetime_to_str(datetime_str, size, time);
    return ESP_OK;
}


/**************************************************************************************************
 *
 * BQ27220 BAT Function
 *
 **************************************************************************************************/
static esp_err_t bsp_bat_create(uint16_t mah)
{
    static parameter_cedv_t default_cedv = {
        .full_charge_cap = 650,
        .design_cap = 650,
        .reserve_cap = 0,
        .near_full = 100,
        .self_discharge_rate = 10,
        .EDV0 = 3200,
        .EDV1 = 3300,
        .EDV2 = 3400,
        .EMF = 3670,
        .C0 = 115,
        .R0 = 968,
        .T0 = 4547,
        .R1 = 4764,
        .TC = 11,
        .C1 = 0,
        .DOD0   = 4200,
        .DOD10  = 4100,
        .DOD20  = 4000,
        .DOD30  = 3920,
        .DOD40  = 3850,
        .DOD50  = 3800,
        .DOD60  = 3750,
        .DOD70  = 3700,
        .DOD80  = 3600,
        .DOD90  = 3450,
        .DOD100 = 3200,
    };
    static const gauging_config_t default_config = {
        .CCT = 1,
        .CSYNC = 0,
        .EDV_CMP = 0,
        .SC = 1,
        .FIXED_EDV0 = 0,
        .FCC_LIM = 1,
        .FC_FOR_VDQ = 1,
        .IGNORE_SD = 1,
        .SME0 = 0,
    };
    bq27220_config_t bq27220_cfg = {
        .i2c_bus = i2c_bus,
        .cfg = &default_config,
        .cedv = &default_cedv,
    };

    default_cedv.full_charge_cap = mah;
    default_cedv.design_cap = mah;

    if (bq27220_i2c_dev != NULL) {
        i2c_bus_device_delete(&bq27220_i2c_dev);
    }
    if (bq27220 != NULL) {
        bq27220_delete(bq27220);
        bq27220 = NULL;
    }

    bq27220_i2c_dev = i2c_bus_device_create(i2c_bus, 0x55, 0);
    if (bq27220_i2c_dev == NULL) {
        bq27220_initialized = false;
        return ESP_FAIL;
    }

    bq27220 = bq27220_create(&bq27220_cfg);
    bq27220_initialized = (bq27220 != NULL);
    bq27220_capacity_mah = mah;

    if (!bq27220_initialized) {
        i2c_bus_device_delete(&bq27220_i2c_dev);
    }

    return bq27220_initialized ? ESP_OK : ESP_FAIL;
}

static esp_err_t bsp_bat_recover(void)
{
    if (bq27220_capacity_mah == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Reinitializing BQ27220");
    return bsp_bat_create(bq27220_capacity_mah);
}

static bool bsp_bat_sample_is_valid(const bsp_bq27220_measurement_t *measurement)
{
    if (measurement == NULL) {
        return false;
    }

    return (measurement->voltage >= 2500) &&
        (measurement->voltage <= 5000) &&
        (measurement->state_of_charge <= 100) &&
        (measurement->temperature >= 2000) &&
        (measurement->temperature <= 4000);
}

static bool bsp_bat_sample_is_transient(const bsp_bq27220_measurement_t *measurement)
{
    if (measurement == NULL) {
        return false;
    }

    return (measurement->current == 0) &&
        (measurement->state_of_charge == 0) &&
        (measurement->remaining_capacity == 0) &&
        (measurement->full_charge_capacity > 0) &&
        (measurement->voltage >= 3500) &&
        (measurement->voltage <= 3900);
}

static esp_err_t bsp_bat_read_u16(uint8_t reg_addr, uint16_t *value)
{
    esp_err_t ret = ESP_FAIL;
    uint8_t retry = 0;

    if ((value == NULL) || (bq27220_i2c_dev == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (retry < 3) {
        ret = i2c_bus_read_bytes(bq27220_i2c_dev, reg_addr, 2, (uint8_t *)value);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        retry++;
        if (retry < 3) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    return ret;
}

static esp_err_t bsp_bat_get_measurement(bsp_bq27220_measurement_t *measurement)
{
    if (measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x08, &measurement->voltage), TAG, "Failed to read voltage");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x0C, (uint16_t *)&measurement->current), TAG, "Failed to read current");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x10, &measurement->remaining_capacity), TAG, "Failed to read remaining capacity");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x12, &measurement->full_charge_capacity), TAG, "Failed to read full charge capacity");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x06, &measurement->temperature), TAG, "Failed to read temperature");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x2A, &measurement->cycle_count), TAG, "Failed to read cycle count");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x2C, &measurement->state_of_charge), TAG, "Failed to read state of charge");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x24, (uint16_t *)&measurement->average_power), TAG, "Failed to read average power");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x1E, (uint16_t *)&measurement->maxload_current), TAG, "Failed to read max load current");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x16, &measurement->time_to_empty), TAG, "Failed to read time to empty");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x18, &measurement->time_to_full), TAG, "Failed to read time to full");
    ESP_RETURN_ON_ERROR(bsp_bat_read_u16(0x2E, &measurement->state_of_health), TAG, "Failed to read state of health");

    return ESP_OK;
}

esp_err_t bsp_bat_init(uint16_t mah)
{
    if (!i2c_initialized)
    {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    }
    // esp_err_t ret = bq27220_init(i2c_handle, BSP_BAT_I2C_ADDRESS, 400 * 1000, &gauge);
    
    // // Ejemplo avanzado: configurar capacidad y (opcional) EDVs vía DM/Config Update
    // // NOTA: Para DM suele requerirse UNSEAL / FULL ACCESS con tus claves reales.
    // // Sustituye 0x0414/0x3672 por tus claves si procede.
    // (void)bq27220_unseal(gauge, 0x0414, 0x3672);
    // ESP_LOGI(TAG, "bq27220_unseal");
    // vTaskDelay(pdMS_TO_TICKS(200));
    // (void)bq27220_full_access(gauge, 0xFFFF, 0xFFFF);
    // ESP_LOGI(TAG, "bq27220_full_access");
    // // Set Design Capacity por comando directo
    // // ESP_ERROR_CHECK(bq27220_set_design_capacity_mah(gauge, mah));
    // // ESP_LOGI(TAG, "bq27220_set_design_capacity_mah");
    // // (Opcional) Entrar en CONFIG_UPDATE y escribir EDVs en mV
    // ESP_ERROR_CHECK(bq27220_begin_config_update(gauge));

    // bq27220_write_data_memory_u16(gauge, BQ27220_DM_ADDR_PROFILE1_DESIGN_CAPACITY, mah);
    // //ESP_ERROR_CHECK(bq27220_set_edvs_mv(gauge, 3200, 3300, 3400));

    // ESP_ERROR_CHECK(bq27220_end_config_update(gauge, true));
    
    // ESP_ERROR_CHECK(bq27220_set_design_capacity_mah(gauge, mah));
    // ESP_LOGI(TAG, "bq27220_set_design_capacity_mah");
    return bsp_bat_create(mah);
}

esp_err_t bsp_get_bat_info(bsp_bat_info_t *bat_info)
{
    esp_err_t ret = ESP_FAIL;
    bsp_bq27220_measurement_t measurement = {0};
    uint8_t retry = 0;

    if (bat_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bq27220_initialized || (bq27220 == NULL)) {
        ESP_RETURN_ON_ERROR(bsp_bat_recover(), TAG, "Failed to recover battery gauge");
    }

    while (retry < 3) {
        ret = bsp_bat_get_measurement(&measurement);
        if ((ret == ESP_OK) && bsp_bat_sample_is_valid(&measurement)) {
            if (bsp_bat_sample_is_transient(&measurement)) {
                retry++;
                if (retry >= 3) {
                    ret = ESP_ERR_INVALID_STATE;
                    break;
                }
                ret = bsp_bat_recover();
                if (ret != ESP_OK) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            bat_info->mv = measurement.voltage;
            bat_info->ma = measurement.current;
            bat_info->mah_rem = measurement.remaining_capacity;
            bat_info->mah_fcc = measurement.full_charge_capacity;
            bat_info->tc = measurement.temperature / 10 - 273;
            bat_info->cycles = measurement.cycle_count;
            bat_info->soc = measurement.state_of_charge;
            bat_info->mw = measurement.average_power;
            bat_info->max_load = measurement.maxload_current;
            bat_info->tte = measurement.time_to_empty;
            bat_info->ttf = measurement.time_to_full;
            bat_info->soh = measurement.state_of_health;
            return ESP_OK;
        }

        retry++;
        if (retry >= 3) {
            break;
        }
        if (retry == 1) {
            ret = bsp_bat_recover();
            if (ret != ESP_OK) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "Failed to refresh battery info");
    return (ret == ESP_OK) ? ESP_ERR_INVALID_STATE : ret;
}

bool bsp_bat_is_connected(void)
{
    const int16_t current_ma = (!bq27220_initialized || (bq27220 == NULL)) ? 0 : bq27220_get_current(bq27220);

    return bq27220_initialized &&
        (bq27220 != NULL) &&
        ((current_ma > 0) ||
        (current_ma < -0));
}

/**************************************************************************************************
 *
 * QMI8658 Function
 *
 **************************************************************************************************/

esp_err_t bsp_qmi8658_init()
{
    ESP_LOGI(TAG, "Initializing QMI8658...");
    esp_err_t ret = qmi8658_init(&qmi8658_dev, i2c_handle, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMI8658 (error: %d)", ret);
        vTaskDelete(NULL);
    }
    qmi8658_set_accel_range(&qmi8658_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&qmi8658_dev, QMI8658_ACCEL_ODR_1000HZ);
    qmi8658_set_gyro_range(&qmi8658_dev, QMI8658_GYRO_RANGE_512DPS);    
    qmi8658_set_gyro_odr(&qmi8658_dev, QMI8658_GYRO_ODR_1000HZ);
    
    qmi8658_set_accel_unit_mps2(&qmi8658_dev, true);
    qmi8658_set_gyro_unit_rads(&qmi8658_dev, true);
    
    qmi8658_set_display_precision(&qmi8658_dev, 4);

    return ret;
}

esp_err_t bsp_qmi8658_get_data(qmi8658_data_t *data)
{
    bool ready = false;
    esp_err_t ret = ESP_FAIL;   // ✅ 关键：先初始化

    ret = qmi8658_is_data_ready(&qmi8658_dev, &ready);
    if (ret == ESP_OK && ready) {
        ret = qmi8658_read_sensor_data(&qmi8658_dev, data);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Accel: X=%.4f m/s², Y=%.4f m/s², Z=%.4f m/s²",
                        data->accelX, data->accelY, data->accelZ);
            ESP_LOGI(TAG, "Gyro:  X=%.4f rad/s, Y=%.4f rad/s, Z=%.4f rad/s",
                        data->gyroX, data->gyroY, data->gyroZ);
            ESP_LOGI(TAG, "Temp:  %.2f °C, Timestamp: %lu",
                        data->temperature, data->timestamp);
            ESP_LOGI(TAG, "----------------------------------------");
        } else {
            ESP_LOGE(TAG, "Failed to read sensor data (error: %d)", ret);
        }
    } else {
        ESP_LOGE(TAG, "Data not ready or error reading status (error: %d)", ret);
    }

    return ret;
}
