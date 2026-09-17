#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
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

#define WIFI_AP_SSID       "Heltec_ESP32"
#define WIFI_AP_PASS       "12345678"
#define MAX_STA_CONN       4

#define RPM_LIMIT_MIN 5000.0f
#define RPM_LIMIT_MAX 45000.0f

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// Глобальные переменные
static volatile float g_current_temp = 0.0f;
static volatile float g_current_rpm  = 0.0f;
static volatile bool  g_bmp_ok       = false;

// Глобальный хендл веб-сервера для широковещательной рассылки WS
static httpd_handle_t g_server = NULL;

void heltec_board_init() {
    gpio_reset_pin(VEXT_PIN); gpio_set_direction(VEXT_PIN, GPIO_MODE_OUTPUT); gpio_set_level(VEXT_PIN, 0);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_reset_pin(OLED_RST); gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT); gpio_set_level(OLED_RST, 0);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    gpio_set_level(OLED_RST, 1);
    vTaskDelay(50 / portTICK_PERIOD_MS);
}

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
// WebSockets Асинхронная рассылка
// ==========================================
// Функция выполняется в контексте потока HTTP-сервера, собирает данные 
// и отправляет всем подключенным по WS клиентам
static void broadcast_ws_data_work(void *arg) {
    if (!g_server) return;

    char buf[128];
    int len = snprintf(buf, sizeof(buf),
        "{\"max_rpm\":%.0f,\"current_rpm\":%.0f,\"temp\":%.1f}",
        motor610_max_rpm, g_current_rpm, g_current_temp);

    httpd_ws_frame_t ws_pkt = {
        .payload = (uint8_t*)buf,
        .len = len,
        .type = HTTPD_WS_TYPE_TEXT
    };

    size_t max_clients = 8;
    int client_fds[8];
    if (httpd_get_client_list(g_server, &max_clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < max_clients; i++) {
            int sock = client_fds[i];
            if (httpd_ws_get_fd_info(g_server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(g_server, sock, &ws_pkt);
            }
        }
    }
}

// Обработчик WebSocket эндпоинта /ws
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Первичное рукопожатие, сразу после подключения ставим в очередь задачу 
        // отправить новому клиенту свежие данные.
        httpd_queue_work(g_server, broadcast_ws_data_work, NULL);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[64] = { 0 };
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len > 0) {
        // Парсим текстовую команду от ползунка браузера, например "rpm:25000"
        if (strncmp((char*)ws_pkt.payload, "rpm:", 4) == 0) {
            float new_rpm = atof((char*)ws_pkt.payload + 4);
            if (new_rpm < RPM_LIMIT_MIN) new_rpm = RPM_LIMIT_MIN;
            if (new_rpm > RPM_LIMIT_MAX) new_rpm = RPM_LIMIT_MAX;
            
            motor610_max_rpm = new_rpm;

            // Немедленно уведомляем всех остальных о новом лимите
            httpd_queue_work(g_server, broadcast_ws_data_work, NULL);
        }
    }
    return ESP_OK;
}

// Отдача главной HTML-страницы
static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

void wifi_ap_init_and_start_webserver(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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
    wifi_config_t sta_config = { .sta = { .ssid = "Wokwi-GUEST", .password = "" } };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    if (httpd_start(&g_server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = get_handler };
        httpd_register_uri_handler(g_server, &uri_get);

        httpd_uri_t uri_ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true // ВАЖНО: флаг поддержки WS
        };
        httpd_register_uri_handler(g_server, &uri_ws);
        ESP_LOGI(TAG, "Web-сервер и WebSocket запущены");
    }
}

// ==========================================
// Задачи FreeRTOS
// ==========================================
static void bmp280_task(void *pvParameters) {
    while (1) {
        if (!g_bmp_ok) g_bmp_ok = (bmp280_init(I2C_MASTER_NUM) == ESP_OK);
        if (g_bmp_ok) {
            float temperature = 0.0f;
            if (bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
                g_current_temp = temperature;
            } else {
                g_bmp_ok = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void motor_task(void *pvParameters) {
    float last_b_temp = -1.0f;
    float last_b_rpm = -1.0f;
    float last_b_max = -1.0f;
    TickType_t last_broadcast = 0;

    while (1) {
        if (g_bmp_ok) {
            float target_rpm = motor610_target_rpm_for_temp(g_current_temp);
            motor610_set_rpm(target_rpm);
            g_current_rpm = target_rpm;
        } else {
            motor610_set_rpm(0.0f);
            g_current_rpm = 0.0f;
        }

        // --- УМНАЯ РАССЫЛКА ДАННЫХ WS (Push Events) ---
        float diff_temp = g_current_temp - last_b_temp;
        float diff_rpm = g_current_rpm - last_b_rpm;
        
        // Отправляем данные только если они существенно изменились
        bool changed = (diff_temp < -0.05f || diff_temp > 0.05f) ||
                       (diff_rpm < -1.0f || diff_rpm > 1.0f) ||
                       (motor610_max_rpm != last_b_max);
                       
        TickType_t now = xTaskGetTickCount();
        
        // Или если прошла 1 секунда (Keep-Alive для спокойствия фронтенда)
        if (changed || (now - last_broadcast > pdMS_TO_TICKS(1000))) {
            last_b_temp = g_current_temp;
            last_b_rpm = g_current_rpm;
            last_b_max = motor610_max_rpm;
            last_broadcast = now;
            
            // Мгновенно ставим задачу на рассылку для потока Web-сервера
            if (g_server) httpd_queue_work(g_server, broadcast_ws_data_work, NULL);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void oled_task(void *pvParameters) {
    ssd1306_handle_t oled = (ssd1306_handle_t)pvParameters;
    while (1) {
        if (oled != NULL) {
            ssd1306_clear_screen(oled, 0x00);
            if (g_bmp_ok) {
                char t_str[32], r_str[32], m_str[32];
                snprintf(t_str, sizeof(t_str), "Temp: %.1f C", g_current_temp);
                snprintf(r_str, sizeof(r_str), "Fan: %.0f RPM", g_current_rpm);
                snprintf(m_str, sizeof(m_str), "Max: %.0f RPM", motor610_max_rpm);
                ssd1306_draw_string(oled, 0, 0, (const uint8_t *)t_str, 16, 1);
                ssd1306_draw_string(oled, 0, 20, (const uint8_t *)r_str, 16, 1);
                ssd1306_draw_string(oled, 0, 40, (const uint8_t *)m_str, 16, 1);
            } else {
                ssd1306_draw_string(oled, 0, 16, (const uint8_t *)"BMP ERROR", 16, 1);
            }
            ssd1306_refresh_gram(oled);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{    
    heltec_board_init();
    wifi_ap_init_and_start_webserver();
    i2c_master_init();

    ssd1306_handle_t oled = ssd1306_create(I2C_MASTER_NUM, OLED_ADDR);
    if (oled != NULL) ssd1306_refresh_gram(oled);

    motor610_init();

    xTaskCreate(bmp280_task, "bmp", 4096, NULL, 5, NULL);
    xTaskCreate(motor_task,  "mot", 4096, NULL, 5, NULL);
    xTaskCreate(oled_task,   "oled",4096, oled, 5, NULL);
}