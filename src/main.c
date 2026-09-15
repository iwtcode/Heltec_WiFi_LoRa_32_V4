#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h" 
#include "driver/gpio.h"
#include "bmp280.h"

static const char *TAG = "HELTEC_V4";

#define I2C_MASTER_SDA_IO 17
#define I2C_MASTER_SCL_IO 18
#define I2C_MASTER_NUM 0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_ADDR 0x3C 

// Специфичные пины платы Heltec V3/V4
#define VEXT_PIN GPIO_NUM_36
#define OLED_RST GPIO_NUM_21

// Функция включения питания платы и сброса экрана
void heltec_board_init() {
    // 1. Включаем питание Vext (активируется низким уровнем - 0)
    gpio_reset_pin(VEXT_PIN);
    gpio_set_direction(VEXT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VEXT_PIN, 0); 
    vTaskDelay(50 / portTICK_PERIOD_MS);

    // 2. Аппаратный сброс OLED экрана
    gpio_reset_pin(OLED_RST);
    gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST, 0); // Притягиваем Reset к земле
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_set_level(OLED_RST, 1); // Отпускаем Reset
    vTaskDelay(50 / portTICK_PERIOD_MS);
}

// Функция инициализации I2C
void i2c_master_init() {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// Минимальная инициализация SSD1306
void oled_init() {
    uint8_t init_cmds[] = {
        0x00, 0xAE, 0x20, 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07, 
        0x81, 0xCF, 0xA1, 0xA6, 0xA8, 0x3F, 0xC8, 0xD3, 0x00, 0xD5, 
        0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40, 0x8D, 0x14, 0xAF
    };
    i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, init_cmds, sizeof(init_cmds), 1000 / portTICK_PERIOD_MS);
}

// Заливка экрана паттерном
void oled_fill() {
    uint8_t data[129];
    data[0] = 0x40;
    for(int i=1; i<129; i++) data[i] = 0xFF;
    
    for(int page=0; page<8; page++) {
        i2c_master_write_to_device(I2C_MASTER_NUM, OLED_ADDR, data, sizeof(data), 1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    heltec_board_init();

    ESP_LOGI(TAG, "Запуск прошивки ESP-IDF для Heltec LoRa V3/V4");

    ESP_LOGI(TAG, "Инициализация I2C...");
    i2c_master_init();

    ESP_LOGI(TAG, "Инициализация OLED...");
    oled_init();
    
    oled_fill();

    ESP_LOGI(TAG, "Инициализация датчика BMP280...");
    esp_err_t bmp_err = bmp280_init(I2C_MASTER_NUM);
    if (bmp_err != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 не инициализирован (%s). Показания температуры недоступны.",
                 esp_err_to_name(bmp_err));
    }

    int counter = 0;
    while (1) {
        float temperature = 0.0f;
        if (bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
            ESP_LOGI(TAG, "Температура (BMP280): %.2f °C | счетчик: %d", temperature, counter);
        } else {
            ESP_LOGW(TAG, "Не удалось прочитать температуру с BMP280 | счетчик: %d", counter);
        }
        counter++;
        vTaskDelay(1000 / portTICK_PERIOD_MS); 
    }
}
