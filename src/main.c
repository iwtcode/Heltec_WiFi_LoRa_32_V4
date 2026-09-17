#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "bmp280.h"
#include "motor610.h"
#include "ssd1306.h"

// Подключаем библиотеки для Wi-Fi и HTTP
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"

static const char *TAG = "HELTEC_V4";

#define I2C_MASTER_SDA_IO 17
#define I2C_MASTER_SCL_IO 18
#define I2C_MASTER_NUM 0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_ADDR 0x3C

#define VEXT_PIN GPIO_NUM_36
#define OLED_RST GPIO_NUM_21

// Настройки Wi-Fi
#define WIFI_AP_SSID       "Heltec_ESP32"
#define WIFI_AP_PASS       "12345678"
#define MAX_STA_CONN       4

// Допустимый диапазон пользовательского лимита RPM (совпадает со слайдером в веб-интерфейсе)
#define RPM_LIMIT_MIN 5000.0f
#define RPM_LIMIT_MAX 45000.0f

// Символы генерируются линкером из имени файла в EMBED_TXTFILES
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Последние измеренные значения, доступные HTTP-обработчикам (обновляются в главном цикле)
static volatile float g_current_temp = 0.0f;
static volatile float g_current_rpm  = 0.0f;

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

// ==========================================
// HTTP Обработчики
// ==========================================

// Отправка главной HTML-страницы
static esp_err_t get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                     index_html_end - index_html_start);
    return ESP_OK;
}

// Отдаёт текущий лимит RPM, фактические обороты вентилятора и температуру
static esp_err_t rpm_handler(httpd_req_t *req)
{
    char buf[96];
    int len = snprintf(buf, sizeof(buf),
        "{\"max_rpm\":%.0f,\"current_rpm\":%.0f,\"temp\":%.1f}",
        motor610_max_rpm, g_current_rpm, g_current_temp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// Обработка отправки формы / запроса от слайдера
static esp_err_t set_handler(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        // Ищем параметр rpm=
        if (httpd_query_key_value(buf, "rpm", param, sizeof(param)) == ESP_OK) {
            float new_rpm = atof(param);
            if (new_rpm < RPM_LIMIT_MIN) new_rpm = RPM_LIMIT_MIN;
            if (new_rpm > RPM_LIMIT_MAX) new_rpm = RPM_LIMIT_MAX;
            motor610_max_rpm = new_rpm;
            ESP_LOGI(TAG, "Установлен новый лимит RPM: %.0f", motor610_max_rpm);
        }
    }
    // Перенаправление обратно на главную страницу (код 303)
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ==========================================
// Инициализация Wi-Fi (AP + STA) и Web-сервера
// ==========================================
void wifi_ap_init_and_start_webserver(void)
{
    // 1. Инициализация NVS-памяти (требуется для Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Инициализация сетевого стека
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Создаем оба интерфейса: и AP (точка доступа), и STA (клиент для симулятора Wokwi)
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3. Конфигурация точки доступа (для реального телефона)
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = 1,
            .password = WIFI_AP_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    // 4. Конфигурация клиента (для проброса портов в Wokwi)
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "Wokwi-GUEST",
            .password = "",
        },
    };

    // Включаем комбинированный режим: AP + STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Пытаемся подключиться к Wokwi-сети (на реальном железе просто будет тихая ошибка, AP при этом продолжит работать)
    esp_wifi_connect();

    ESP_LOGI(TAG, "Точка доступа Wi-Fi запущена. SSID: %s, пароль: %s", WIFI_AP_SSID, WIFI_AP_PASS);

    // 5. Запуск HTTP-сервера
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // В ESP-IDF по умолчанию HTTP-сервер слушает все интерфейсы (и AP, и STA)
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_rpm = { .uri = "/rpm", .method = HTTP_GET, .handler = rpm_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_rpm);

        httpd_uri_t uri_set = { .uri = "/set", .method = HTTP_GET, .handler = set_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_set);
        ESP_LOGI(TAG, "Web-сервер запущен");
    }
}

void app_main(void)
{
    heltec_board_init();

    ESP_LOGI(TAG, "Запуск прошивки ESP-IDF для Heltec LoRa V3/V4");

    // Поднимаем AP и запускаем Web-сервер
    wifi_ap_init_and_start_webserver();

    ESP_LOGI(TAG, "Инициализация I2C...");
    i2c_master_init();

    ESP_LOGI(TAG, "Инициализация OLED через библиотеку...");
    ssd1306_handle_t oled = ssd1306_create(I2C_MASTER_NUM, OLED_ADDR);

    ssd1306_clear_screen(oled, 0x00);
    ssd1306_refresh_gram(oled);

    ESP_LOGI(TAG, "Инициализация датчика BMP280...");
    esp_err_t bmp_err = bmp280_init(I2C_MASTER_NUM);
    if (bmp_err != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 не инициализирован (%s)", esp_err_to_name(bmp_err));
        ssd1306_draw_string(oled, 0, 16, (const uint8_t *)"BMP280 ERROR", 16, 1);
        ssd1306_refresh_gram(oled);
    }

    ESP_LOGI(TAG, "Инициализация ШИМ вентилятора motor610 (пин %d)...", MOTOR610_PWM_PIN);
    motor610_init();
    motor610_set_rpm(0.0f);

    int counter = 0;
    while (1) {
        float temperature = 0.0f;
        if (bmp_err == ESP_OK && bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
            float target_rpm = motor610_target_rpm_for_temp(temperature);
            motor610_set_rpm(target_rpm);

            // Обновляем значения, которые отдаёт веб-сервер (/rpm)
            g_current_temp = temperature;
            g_current_rpm  = target_rpm;

            // 1. Очищаем виртуальный буфер дисплея
            ssd1306_clear_screen(oled, 0x00);

            // 2. Формируем строки с текстом
            char temp_str[32];
            char rpm_str[32];
            char maxrpm_str[32];
            snprintf(temp_str, sizeof(temp_str), "Temp: %.1f C", temperature);
            snprintf(rpm_str, sizeof(rpm_str), "Fan: %.0f RPM", target_rpm);
            snprintf(maxrpm_str, sizeof(maxrpm_str), "Max: %.0f", motor610_max_rpm);

            // 3. Рисуем текст в буфере
            ssd1306_draw_string(oled, 0, 0, (const uint8_t *)temp_str, 16, 1);
            ssd1306_draw_string(oled, 0, 20, (const uint8_t *)rpm_str, 16, 1);
            ssd1306_draw_string(oled, 0, 40, (const uint8_t *)maxrpm_str, 16, 1);

            // 4. Отправляем буфер на физический экран по I2C
            ssd1306_refresh_gram(oled);
        } else {
            ESP_LOGW(TAG, "Не удалось прочитать температуру | счетчик: %d", counter);
        }
        counter++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}