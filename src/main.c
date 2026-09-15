#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h" 
#include "driver/gpio.h"
#include "bmp280.h"
#include "ssd1306.h" // Подключаем скачанную библиотеку

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
    gpio_reset_pin(VEXT_PIN);
    gpio_set_direction(VEXT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VEXT_PIN, 0); 
    vTaskDelay(50 / portTICK_PERIOD_MS);

    gpio_reset_pin(OLED_RST);
    gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST, 0); 
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_set_level(OLED_RST, 1); 
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

void app_main(void)
{
    heltec_board_init();

    ESP_LOGI(TAG, "Запуск прошивки ESP-IDF для Heltec LoRa V3/V4");

    ESP_LOGI(TAG, "Инициализация I2C...");
    i2c_master_init();

    ESP_LOGI(TAG, "Инициализация OLED через библиотеку...");
    // Передаем библиотеке уже готовый порт I2C и адрес устройства (0x3C)
    ssd1306_handle_t oled = ssd1306_create(I2C_MASTER_NUM, OLED_ADDR);
    
    // Очищаем экран (заполняем черным цветом)
    ssd1306_clear_screen(oled, 0x00);
    ssd1306_refresh_gram(oled);

    ESP_LOGI(TAG, "Инициализация датчика BMP280...");
    esp_err_t bmp_err = bmp280_init(I2C_MASTER_NUM);
    if (bmp_err != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 не инициализирован (%s)", esp_err_to_name(bmp_err));
        ssd1306_draw_string(oled, 0, 16, (const uint8_t *)"BMP280 ERROR", 16, 1);
        ssd1306_refresh_gram(oled);
    }

    int counter = 0;
    while (1) {
        float temperature = 0.0f;
        if (bmp_err == ESP_OK && bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
            ESP_LOGI(TAG, "Температура (BMP280): %.2f °C | счетчик: %d", temperature, counter);
            
            // 1. Очищаем виртуальный буфер дисплея
            ssd1306_clear_screen(oled, 0x00);
            
            // 2. Формируем строки с текстом
            char temp_str[32];
            char cnt_str[32];
            snprintf(temp_str, sizeof(temp_str), "Temp: %.1f C", temperature);
            snprintf(cnt_str, sizeof(cnt_str), "Count: %d", counter);
            
            // 3. Рисуем текст в буфере 
            // Параметры: хэндл, X, Y, текст, размер шрифта (16), цвет (1 - белый)
            ssd1306_draw_string(oled, 0, 16, (const uint8_t *)temp_str, 16, 1);
            ssd1306_draw_string(oled, 0, 40, (const uint8_t *)cnt_str, 16, 1);
            
            // 4. Отправляем буфер на физический экран по I2C
            ssd1306_refresh_gram(oled);
        } else {
            ESP_LOGW(TAG, "Не удалось прочитать температуру | счетчик: %d", counter);
        }
        counter++;
        vTaskDelay(2000 / portTICK_PERIOD_MS); 
    }
}