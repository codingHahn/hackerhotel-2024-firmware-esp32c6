#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "drv2605.h"
#include "epaper.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hextools.h"
#include "managed_i2c.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "sid.h"

#include <stdio.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <sdmmc_cmd.h>
#include <string.h>
#include <time.h>

#define I2C_BUS     0
#define I2C_SPEED   400000 // 400 kHz
#define I2C_TIMEOUT 250    // us

#define GPIO_I2C_SCL 7
#define GPIO_I2C_SDA 6

static char const *TAG = "main";

i2s_chan_handle_t i2s_handle = NULL;

pax_buf_t gfx;
pax_col_t palette[] = {0xffffffff, 0xffff0000, 0xff000000};

HINK epaper = {
    .spi_bus               = SPI2_HOST,
    .pin_cs                = 8,
    .pin_dcx               = 5,
    .pin_reset             = 16,
    .pin_busy              = 10,
    .spi_speed             = 10000000,
    .spi_max_transfer_size = SOC_SPI_MAXIMUM_BUFFER_SIZE,
};

drv2605_t drv2605_device = {
    .i2c_bus     = I2C_BUS,
    .i2c_address = DRV2605_ADDR,
};

sdmmc_card_t *card = NULL;

static esp_err_t initialize_nvs(void) {
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK)
            return res;
        res = nvs_flash_init();
    }
    return res;
}

static esp_err_t initialize_system() {
    esp_err_t res;

    // Non-volatile storage
    res = initialize_nvs();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing NVS failed");
        return res;
    }

    // I2C bus
    i2c_config_t i2c_config = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = GPIO_I2C_SDA,
        .scl_io_num       = GPIO_I2C_SCL,
        .master.clk_speed = I2C_SPEED,
        .sda_pullup_en    = false,
        .scl_pullup_en    = false,
        .clk_flags        = 0
    };

    res = i2c_param_config(I2C_BUS, &i2c_config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Configuring I2C bus parameters failed");
        return res;
    }

    res = i2c_set_timeout(I2C_BUS, I2C_TIMEOUT * 80);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Configuring I2C bus timeout failed");
        // return res;
    }

    res = i2c_driver_install(I2C_BUS, i2c_config.mode, 0, 0, 0);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing I2C bus failed");
        return res;
    }

    // I2S audio
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    res = i2s_new_channel(&chan_cfg, &i2s_handle, NULL);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing I2S channel failed");
        return res;
    }

    i2s_std_config_t i2s_config = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = GPIO_NUM_23,
                .ws   = GPIO_NUM_17,
                .dout = GPIO_NUM_22,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
            },
    };

    res = i2s_channel_init_std_mode(i2s_handle, &i2s_config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Configuring I2S channel failed");
        return res;
    }

    res = i2s_channel_enable(i2s_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Enabling I2S channel failed");
        return res;
    }

    // GPIO for controlling power to the audio amplifier
    gpio_config_t pin_amp_enable_cfg = {
        .pin_bit_mask = 1 << 1,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = false,
        .pull_down_en = false,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_set_level(1, true);
    gpio_config(&pin_amp_enable_cfg);

    // SPI bus
    spi_bus_config_t busConfiguration = {0};
    busConfiguration.mosi_io_num      = 19;
    busConfiguration.miso_io_num      = 20;
    busConfiguration.sclk_io_num      = 21;
    busConfiguration.quadwp_io_num    = -1;
    busConfiguration.quadhd_io_num    = -1;
    busConfiguration.max_transfer_sz  = SOC_SPI_MAXIMUM_BUFFER_SIZE;

    res = spi_bus_initialize(SPI2_HOST, &busConfiguration, SPI_DMA_CH_AUTO);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing SPI bus failed");
        return res;
    }

    // Epaper display
    res = hink_init(&epaper);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing epaper display failed");
        return res;
    }

    // DRV2605 vibration motor driver
    res = drv2605_init(&drv2605_device);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing DRV2605 failed");
        return res;
    }

    // Graphics stack
    ESP_LOGI(TAG, "Creating graphics...");
    pax_buf_init(&gfx, NULL, 152, 152, PAX_BUF_2_PAL);
    gfx.palette      = palette;
    gfx.palette_size = sizeof(palette) / sizeof(pax_col_t);

    // SD card
    sdmmc_host_t          host        = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = 18;
    slot_config.host_id               = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config =
        {.format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

    res = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Initializing SD card failed");
        card = NULL;
    }

    // SID emulator
    res = sid_init(i2s_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Initializing SID emulator failed");
        return res;
    }

    return res;
}

void test_time() {
    time_t    now;
    char      strftime_buf[64];
    struct tm timeinfo;

    time(&now);
    // Set timezone to China Standard Time
    setenv("TZ", "CST-8", 1);
    tzset();

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in Shanghai is: %s", strftime_buf);
}

void app_main(void) {
    esp_err_t res = initialize_system();
    if (res != ESP_OK) {
        // Device init failed, stop.
        return;
    }

    if (card) {
        sdmmc_card_print_info(stdout, card);
    }

    pax_background(&gfx, 0);
    pax_set_pixel(&gfx, 1, 5, 5);
    pax_set_pixel(&gfx, 2, 5, 10);

    pax_draw_rect(&gfx, 1, 0, 0, 50, 20);
    pax_draw_text(&gfx, 0, pax_font_sky, 18, 1, 1, "Test");
    pax_draw_rect(&gfx, 2, 50, 0, 50, 20);
    pax_draw_text(&gfx, 0, pax_font_sky, 18, 51, 1, "Test");

    pax_draw_rect(&gfx, 0, 0, 20, 50, 20);
    pax_draw_text(&gfx, 1, pax_font_sky, 18, 1, 21, "Test");
    pax_draw_rect(&gfx, 2, 50, 20, 50, 20);
    pax_draw_text(&gfx, 1, pax_font_sky, 18, 51, 21, "Test");

    pax_draw_rect(&gfx, 0, 0, 40, 50, 20);
    pax_draw_text(&gfx, 2, pax_font_sky, 18, 1, 41, "Test");
    pax_draw_rect(&gfx, 1, 50, 40, 50, 20);
    pax_draw_text(&gfx, 2, pax_font_sky, 18, 51, 41, "Test");


    // hink_set_lut(&epaper, lut_alt);
    hink_write(&epaper, gfx.buf);

    while (1) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
