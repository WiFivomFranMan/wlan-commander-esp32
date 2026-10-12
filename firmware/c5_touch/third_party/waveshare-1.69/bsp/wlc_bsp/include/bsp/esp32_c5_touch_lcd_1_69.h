/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP BSP: ESP32-S3-LCD-EV-Board
 */

#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "i2c_bus.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/temperature_sensor.h"
#include "esp_err.h"
#include "bsp/config.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "esp_io_expander_tca9554.h"
#include "pcf85063a.h"
#include "bq27220.h"
#include "qmi8658.h"
#include "esp_codec_dev.h"

/**************************************************************************************************
 *  ESP32-S3-LCD-EV-Board Pinout
 **************************************************************************************************/

/** @defgroup g01_i2c I2C
 *  @brief I2C BSP API
 *  @{
 */
#define BSP_I2C_SCL (GPIO_NUM_9)
#define BSP_I2C_SDA (GPIO_NUM_8)

/** @} */ // end of i2c

/** @defgroup g03_audio Audio
 *  @brief Audio BSP API
 *  @{
 */
#define BSP_I2S_SCLK (GPIO_NUM_1)
#define BSP_I2S_MCLK (GPIO_NUM_0)
#define BSP_I2S_LCLK (GPIO_NUM_3)
#define BSP_I2S_DOUT (GPIO_NUM_10) // To Codec ES8311
#define BSP_I2S_DSIN (GPIO_NUM_2) // From Codec ES8311
#define BSP_POWER_AMP_IO (IO_EXPANDER_PIN_NUM_0)
/** @} */ // end of audio

/** @defgroup g04_display Display and Touch
 *  @brief Display BSP API
 *  @{
 */
/* Display */
#define BSP_LCD_CS   (GPIO_NUM_27) 
#define BSP_LCD_CLK  (GPIO_NUM_23)
#define BSP_LCD_DATA (GPIO_NUM_7)
#define BSP_LCD_DC   (GPIO_NUM_24)

#define BSP_LCD_BACKLIGHT (GPIO_NUM_26)
#define BSP_LCD_RST       (GPIO_NUM_NC)
#define BSP_LCD_TOUCH_RST     (IO_EXPANDER_PIN_NUM_1)
#define BSP_LCD_TOUCH_INT     (GPIO_NUM_5)

/** @} */ // end of display

/* uSD card */
#define BSP_SD_D0  (GPIO_NUM_NC)
#define BSP_SD_CMD (GPIO_NUM_NC)
#define BSP_SD_CLK (GPIO_NUM_NC)
#define BSP_SD_CS  (GPIO_NUM_NC)

/** @} */ // end of uSD card

#define BSP_IO_EXPANDER_I2C_ADDRESS     (ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000)
#define BSP_RTC_I2C_ADDRESS     (PCF85063A_ADDRESS)
#define BSP_BAT_I2C_ADDRESS     (BQ27220_I2C_ADDR)

#ifdef __cplusplus
extern "C"
{
#endif

/** \addtogroup g01_i2c
 *  @{
 */

/**************************************************************************************************
 *
 * I2C Interface
 *
 * There are multiple devices connected to I2C peripheral:
 *  - Codec ES8311 (configuration only)
 *  - ADC ES7210 (configuration only)
 *  - LCD Touch controller
 *  - IO expander chip TCA9554
 **************************************************************************************************/
#define BSP_I2C_NUM (CONFIG_BSP_I2C_NUM)

    /**
     * @brief Init I2C driver
     *
     * @return
     *      - ESP_OK:               On success
     *      - ESP_ERR_INVALID_ARG:  I2C parameter error
     *      - ESP_FAIL:             I2C driver installation error
     *
     */
    esp_err_t bsp_i2c_init(void);

    /**
     * @brief Deinit I2C driver and free its resources
     *
     * @return
     *      - ESP_OK:               On success
     *      - ESP_ERR_INVALID_ARG:  I2C parameter error
     *
     */
    esp_err_t bsp_i2c_deinit(void);

    /**
     * @brief Get I2C driver handle
     *
     * @return
     *      - I2C handle
     */
    i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/** @} */ // end of i2c

/**************************************************************************************************
 *
 * Audio Interface
 *
 **************************************************************************************************/
    /**
     * @brief Initialize audio I2S interface
     *
     * @param[in] i2s_config Custom I2S standard mode configuration, use default when NULL
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);

    /**
     * @brief Control external power amplifier
     *
     * @param[in] enable true to enable speaker amplifier
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_audio_poweramp_enable(bool enable);

    /**
     * @brief Initialize speaker codec device
     *
     * @return Codec device handle, or NULL when initialization failed
     */
    esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

    /**
     * @brief Initialize microphone codec device
     *
     * @return Codec device handle, or NULL when initialization failed
     */
    esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);

/** @defgroup g02_storage SD Card and SPIFFS
 *  @brief SPIFFS and SD card BSP API
 *  @{
 */

/**************************************************************************************************
 *
 * SPIFFS
 *
 * After mounting the SPIFFS, it can be accessed with stdio functions ie.:
 * \code{.c}
 * FILE* f = fopen(BSP_SPIFFS_MOUNT_POINT"/hello.txt", "w");
 * fprintf(f, "Hello World!\n");
 * fclose(f);
 * \endcode
 **************************************************************************************************/
#define BSP_SPIFFS_MOUNT_POINT CONFIG_BSP_SPIFFS_MOUNT_POINT

    /**
     * @brief Mount SPIFFS to virtual file system
     *
     * @return
     *      - ESP_OK on success
     *      - ESP_ERR_INVALID_STATE if esp_vfs_spiffs_register was already called
     *      - ESP_ERR_NO_MEM if memory can not be allocated
     *      - ESP_FAIL if partition can not be mounted
     *      - other error codes
     */
    esp_err_t bsp_spiffs_mount(void);

    /**
     * @brief Unmount SPIFFS from virtual file system
     *
     * @return
     *      - ESP_OK on success
     *      - ESP_ERR_NOT_FOUND if the partition table does not contain SPIFFS partition with given label
     *      - ESP_ERR_INVALID_STATE if esp_vfs_spiffs_unregister was already called
     *      - ESP_ERR_NO_MEM if memory can not be allocated
     *      - ESP_FAIL if partition can not be mounted
     *      - other error codes
     */
    esp_err_t bsp_spiffs_unmount(void);

/** @} */ // end of storage

/**************************************************************************************************
 *
 * IO Expander Interface
 *
 **************************************************************************************************/
/**
 * @brief Init Custom IO expander chip CH32V003
 *
 * @note If the device was already initialized, users can also use it to get handle.
 * @note This function will be called in `bsp_display_start()` when using LCD sub-board 2 with the resolution of 480x480.
 * @note This function will be called in `bsp_audio_init()`.
 *
 * @return Pointer to device handle or NULL when error occurred
 */
esp_io_expander_handle_t bsp_io_expander_init(void);

/**
 * @brief Set IO expander pin level
 *
 * @param pin IO expander pin number
 * @param level Level to set
 *
 * @return ESP_OK on success
 * @return other error codes
 */
esp_err_t bsp_io_expander_set_level(uint8_t pin, uint8_t level);
esp_err_t bsp_io_expander_set_dir(uint8_t pin, uint8_t dir);
esp_err_t bsp_io_expander_get_level(uint8_t pin, uint8_t *level);

/**************************************************************************************************
 *
 * LCD interface
 *
 * LVGL is used as graphics library. LVGL is NOT thread safe, therefore the user must take LVGL mutex
 * by calling bsp_display_lock() before calling any LVGL API (lv_...) and then give the mutex with
 * bsp_display_unlock().
 *
 * If you want to use the display without LVGL, see bsp/display.h API and use BSP version with 'noglib' suffix.
 **************************************************************************************************/
#define BSP_LCD_SPI_NUM            (SPI2_HOST)
#define BSP_LCD_SPI_CLK_HZ         (80 * 1000 * 1000)

#define BSP_LCD_DRAW_BUFF_SIZE     (BSP_LCD_H_RES * CONFIG_BSP_LCD_RGB_BOUNCE_BUFFER_HEIGHT)
#define BSP_LCD_DRAW_BUFF_DOUBLE   (0)

    /**
     * @brief Initialize display
     *
     * @note This function initializes display controller and starts LVGL handling task.
     * @note Users can get LCD panel handle from `user_data` in returned display.
     *
     * @return Pointer to LVGL display or NULL when error occurred
     */
    lv_display_t *bsp_display_start(void);

    /**
     * @brief Initialize display
     *
     * This function initializes SPI, display controller and starts LVGL handling task.
     * LCD backlight must be enabled separately by calling `bsp_display_brightness_set()`
     *
     * @param cfg display configuration
     *
     * @return Pointer to LVGL display or NULL when error occurred
     */
    lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

    /**
     * @brief Configure BSP display partial buffer mode before display startup
     *
     * @note Call this before `bsp_display_start()` or `bsp_display_start_with_config()`.
     * @note When disabled, BSP keeps the original full-frame double buffering path.
     *
     * @param[in] enable        Enable partial buffer mode
     * @param[in] buffer_height Partial buffer height, use 0 for default
     * @return
     *      - ESP_OK                On success
     *      - ESP_ERR_INVALID_STATE Display already started
     *      - ESP_ERR_INVALID_ARG   Invalid height
     */
    esp_err_t bsp_display_set_partial_mode(bool enable, uint16_t buffer_height);

    /**
     * @brief Get pointer to input device (touch, buttons, ...)
     *
     * @note  The LVGL input device is initialized in `bsp_display_start()` function.
     * @note  This function should be called after calling `bsp_display_start()`.
     *
     * @return Pointer to LVGL input device or NULL when not initialized
     */
    lv_indev_t *bsp_display_get_input_dev(void);

    /**
     * @brief Take LVGL mutex
     *
     * @note  Display must be already initialized by calling `bsp_display_start()`
     *
     * @param[in] timeout_ms: Timeout in [ms]. 0 will block indefinitely.
     *
     * @return
     *      - true:  Mutex was taken
     *      - false: Mutex was NOT taken
     */
    esp_err_t bsp_display_lock(uint32_t timeout_ms);

    /**
     * @brief Give LVGL mutex
     *
     * @note  Display must be already initialized by calling `bsp_display_start()`
     *
     */
    void bsp_display_unlock(void);

    /**
     * @brief Rotate screen
     *
     * @note  Display must be already initialized by calling `bsp_display_start()`
     * @note  This function can't work with the anti-tearing function. Please use the `BSP_DISPLAY_LVGL_ROTATION` configuration instead.
     *
     * @param[in] disp:     Pointer to LVGL display
     * @param[in] rotation: Angle of the display rotation
     */
    void bsp_display_rotate(lv_display_t *disp, lv_display_rotation_t rotation);

    /**************************************************************************************************
     *
     * CPU Temperature
     *
     **************************************************************************************************/
    /**
     * @brief Initialize CPU temperature sensor
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_cpu_temp_init();

    /**
     * @brief Get CPU temperature value
     *
     * @param[out] tsens_value   Pointer to store the temperature value
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_get_cpu_temp(float *tsens_value);


    /**************************************************************************************************
     *
     * RTC Interface
     *
     **************************************************************************************************/
    /**
     * @brief Initialize PCF85063A RTC
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_rtc_init();

    /**
     * @brief Set RTC date and time
     *
     * @param[in] time   RTC date and time structure
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_set_rtc_time_date(pcf85063a_datetime_t time);

    /**
     * @brief Set RTC alarm time
     *
     * @param[in] time   RTC alarm time structure
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_set_rtc_alarm_time(pcf85063a_datetime_t time);

    /**
     * @brief Get current RTC date and time
     *
     * @param[out] time   Pointer to store RTC date and time structure
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_get_rtc_time_date(pcf85063a_datetime_t *time);

    /**
     * @brief Get currently set RTC alarm time
     *
     * @param[out] time   Pointer to store RTC alarm time structure
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_get_rtc_alarm_time(pcf85063a_datetime_t *time);

    /**
     * @brief Enable RTC alarm
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_enable_rtc_alarm();

    /**
     * @brief Convert RTC datetime structure to string
     *
     * @param[out] datetime_str   Buffer to store formatted datetime string
     * @param[in]  size           Size of output buffer
     * @param[in]  time           RTC datetime structure
     *
     * @return
     *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
     */
    esp_err_t bsp_datetime_to_str(char *datetime_str, size_t size, pcf85063a_datetime_t time);

    /**************************************************************************************************
     *
     * BQ27220 BAT Function
     *
     **************************************************************************************************/

    /**
     * @brief BSP display configuration structure
     */
    typedef struct {
        uint16_t mv;        // 电压（mV）
        int16_t ma;         // 电流（mA），正值=充电，负值=放电
        int16_t max_load;   // 负载电流（mA）
        int16_t mw;         // 功率（mW）
        uint16_t soc;       // 剩余电量百分比（%）
        uint16_t tte;       // 预计还能用多久（min）
        uint16_t ttf;       // 预计充满还需多久（min）
        uint16_t mah_rem;   // 剩余容量（mAh）
        uint16_t mah_fcc;   // 满充容量（mAh）
        uint16_t cycles;    // 循环次数（cycle count）
        uint16_t soh;       // 健康度（State of Health %）
        uint16_t tc;        // 温度（°C）
    } bsp_bat_info_t;

    esp_err_t bsp_bat_init(uint16_t mah);
    esp_err_t bsp_get_bat_info(bsp_bat_info_t *bat_info);
    bool bsp_bat_is_connected(void);

    esp_err_t bsp_qmi8658_init();
    esp_err_t bsp_qmi8658_get_data(qmi8658_data_t *data);

#ifdef __cplusplus
}
#endif
