#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
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

// --- Вентилятор 610 (coreless motor fan), см. 610_coreless_motor_fan.md ---
// Пин S модуля подключаем к любому ШИМ-способному цифровому пину.
#define FAN_PWM_PIN      GPIO_NUM_4
#define FAN_LEDC_TIMER   LEDC_TIMER_0
#define FAN_LEDC_MODE    LEDC_LOW_SPEED_MODE   // на ESP32-S3 доступен только low-speed режим LEDC
#define FAN_LEDC_CHANNEL LEDC_CHANNEL_0
#define FAN_LEDC_RES     LEDC_TIMER_10_BIT     // разрешение ШИМ: 0..1023
#define FAN_LEDC_FREQ_HZ 5000

#define FAN_MAX_DUTY     ((1 << 10) - 1) // соответствует FAN_LEDC_RES (10 бит)

// Логика зависимости скорости от температуры (см. задание):
//   temp <= FAN_TEMP_OFF  -> вентилятор выключен (0 об/мин)
//   temp >= FAN_TEMP_MAX  -> максимальная скорость (FAN_MAX_RPM)
//   между ними            -> линейная интерполяция
#define FAN_TEMP_OFF  22.0f
#define FAN_TEMP_MAX  30.0f
#define FAN_MAX_RPM   45000.0f // паспортная скорость холостого хода модуля 610

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

// Инициализация аппаратного ШИМ (LEDC) для управления вентилятором.
// Пин S модуля 610 подключается к MOSFET-драйверу на самом модуле, поэтому
// с цифрового пина ESP32 достаточно обычного ШИМ-сигнала небольшой мощности.
void fan_pwm_init() {
    ledc_timer_config_t timer_conf = {
        .speed_mode = FAN_LEDC_MODE,
        .duty_resolution = FAN_LEDC_RES,
        .timer_num = FAN_LEDC_TIMER,
        .freq_hz = FAN_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = FAN_PWM_PIN,
        .speed_mode = FAN_LEDC_MODE,
        .channel = FAN_LEDC_CHANNEL,
        .timer_sel = FAN_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel_conf);
}

// Пересчитывает температуру (°C) в целевую скорость вентилятора (об/мин):
// 22°C и ниже — выключен, 30°C и выше — максимум, между ними — линейно.
float fan_target_rpm_for_temp(float temp_c) {
    if (temp_c <= FAN_TEMP_OFF) {
        return 0.0f;
    }
    if (temp_c >= FAN_TEMP_MAX) {
        return FAN_MAX_RPM;
    }
    float fraction = (temp_c - FAN_TEMP_OFF) / (FAN_TEMP_MAX - FAN_TEMP_OFF);
    return FAN_MAX_RPM * fraction;
}

// Устанавливает скважность ШИМ на пине S по целевым оборотам (0..FAN_MAX_RPM).
void fan_set_rpm(float target_rpm) {
    if (target_rpm < 0.0f) target_rpm = 0.0f;
    if (target_rpm > FAN_MAX_RPM) target_rpm = FAN_MAX_RPM;

    float fraction = target_rpm / FAN_MAX_RPM;
    uint32_t duty = (uint32_t)(fraction * FAN_MAX_DUTY + 0.5f);

    ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
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

    ESP_LOGI(TAG, "Инициализация ШИМ вентилятора (пин %d)...", FAN_PWM_PIN);
    fan_pwm_init();
    fan_set_rpm(0.0f); // на старте вентилятор выключен

    int counter = 0;
    while (1) {
        float temperature = 0.0f;
        if (bmp_err == ESP_OK && bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
            float target_rpm = fan_target_rpm_for_temp(temperature);
            fan_set_rpm(target_rpm);

            ESP_LOGI(TAG, "Температура: %.2f °C | Вентилятор: %.0f об/мин | счетчик: %d",
                     temperature, target_rpm, counter);

            // 1. Очищаем виртуальный буфер дисплея
            ssd1306_clear_screen(oled, 0x00);

            // 2. Формируем строки с текстом
            char temp_str[32];
            char rpm_str[32];
            char cnt_str[32];
            snprintf(temp_str, sizeof(temp_str), "Temp: %.1f C", temperature);
            snprintf(rpm_str, sizeof(rpm_str), "Fan: %.0f RPM", target_rpm);
            snprintf(cnt_str, sizeof(cnt_str), "Count: %d", counter);

            // 3. Рисуем текст в буфере
            // Параметры: хэндл, X, Y, текст, размер шрифта (16), цвет (1 - белый)
            ssd1306_draw_string(oled, 0, 0, (const uint8_t *)temp_str, 16, 1);
            ssd1306_draw_string(oled, 0, 20, (const uint8_t *)rpm_str, 16, 1);
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