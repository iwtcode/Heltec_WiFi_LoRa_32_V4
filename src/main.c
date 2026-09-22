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
#define TARGET_TEMP_MIN 10.0f
#define TARGET_TEMP_MAX 50.0f
#define TARGET_TEMP_DEFAULT 25.0f

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static volatile float g_current_temp = 0.0f;
static volatile float g_current_rpm  = 0.0f;
static volatile bool  g_bmp_ok       = false;
static volatile bool  g_fan_power    = true;
static volatile float g_target_temp  = TARGET_TEMP_DEFAULT;
static volatile bool  g_pid_reset_requested = true;
static volatile bool  g_failsafe_active = false;

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

static void broadcast_ws_data_work(void *arg) {
    if (!g_server) return;

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{\"max_rpm\":%.0f,\"current_rpm\":%.0f,\"temp\":%.1f,\"target_temp\":%.1f,"
        "\"power\":%d,\"bmp_ok\":%d,\"failsafe\":%d}",
        motor610_max_rpm, g_current_rpm, g_current_temp, g_target_temp,
        g_fan_power ? 1 : 0, g_bmp_ok ? 1 : 0, g_failsafe_active ? 1 : 0);

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

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[64] = { 0 };
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf) - 1);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len > 0) {
        if (strncmp((char*)ws_pkt.payload, "init:", 5) == 0) {
            char out_buf[256];
            int out_len = snprintf(out_buf, sizeof(out_buf),
                "{\"max_rpm\":%.0f,\"current_rpm\":%.0f,\"temp\":%.1f,\"target_temp\":%.1f,"
                "\"power\":%d,\"bmp_ok\":%d,\"failsafe\":%d}",
                motor610_max_rpm, g_current_rpm, g_current_temp, g_target_temp,
                g_fan_power ? 1 : 0, g_bmp_ok ? 1 : 0, g_failsafe_active ? 1 : 0);
            
            httpd_ws_frame_t ws_reply = {
                .payload = (uint8_t*)out_buf,
                .len = out_len,
                .type = HTTPD_WS_TYPE_TEXT
            };
            httpd_ws_send_frame(req, &ws_reply);
        }
        else if (strncmp((char*)ws_pkt.payload, "rpm:", 4) == 0) {
            float new_rpm = atof((char*)ws_pkt.payload + 4);
            if (new_rpm < RPM_LIMIT_MIN) new_rpm = RPM_LIMIT_MIN;
            if (new_rpm > RPM_LIMIT_MAX) new_rpm = RPM_LIMIT_MAX;
            motor610_max_rpm = new_rpm;
            httpd_queue_work(req->handle, broadcast_ws_data_work, NULL);
        }
        else if (strncmp((char*)ws_pkt.payload, "power:", 6) == 0) {
            int pwr = atoi((char*)ws_pkt.payload + 6);
            g_fan_power = (pwr > 0);
            g_pid_reset_requested = true;
            httpd_queue_work(req->handle, broadcast_ws_data_work, NULL);
        }
        else if (strncmp((char*)ws_pkt.payload, "target_temp:", 12) == 0) {
            float target = atof((char*)ws_pkt.payload + 12);
            if (target < TARGET_TEMP_MIN) target = TARGET_TEMP_MIN;
            if (target > TARGET_TEMP_MAX) target = TARGET_TEMP_MAX;
            g_target_temp = target;
            g_pid_reset_requested = true;
            httpd_queue_work(req->handle, broadcast_ws_data_work, NULL);
        }
    }
    return ESP_OK;
}

static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    httpd_resp_send(req, (const char *)index_html_start, (index_html_end - index_html_start) - 1);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
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
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    
    if (httpd_start(&g_server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = get_handler };
        httpd_register_uri_handler(g_server, &uri_get);

        httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };
        httpd_register_uri_handler(g_server, &uri_favicon);

        httpd_uri_t uri_ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_server, &uri_ws);
        ESP_LOGI(TAG, "Web-сервер и WebSocket запущены");
    }
}

static void bmp280_task(void *pvParameters) {
    while (1) {
        if (!g_bmp_ok) {
            // Пытаемся (пере)подключить датчик
            g_bmp_ok = (bmp280_init(I2C_MASTER_NUM) == ESP_OK);
        }
        
        if (g_bmp_ok) {
            float temperature = 0.0f;
            if (bmp280_read_temperature(I2C_MASTER_NUM, &temperature) == ESP_OK) {
                g_current_temp = temperature;
            } else {
                g_bmp_ok = false; // Соединение потеряно
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void motor_task(void *pvParameters) {
    float last_b_temp = -1.0f;
    float last_b_rpm = -1.0f;
    float last_b_max = -1.0f;
    float last_b_target = -1000.0f;
    bool  last_b_power = !g_fan_power; 
    bool  last_b_bmp = !g_bmp_ok;
    bool  last_b_failsafe = !g_failsafe_active;
    TickType_t last_broadcast = 0;
    TickType_t last_control_tick = xTaskGetTickCount();

    while (1) {
        TickType_t control_tick = xTaskGetTickCount();
        float dt_seconds = (float)(control_tick - last_control_tick) * portTICK_PERIOD_MS / 1000.0f;
        last_control_tick = control_tick;

        if (g_pid_reset_requested) {
            motor610_pid_reset();
            g_pid_reset_requested = false;
        }

        if (!g_fan_power) {
            motor610_pid_reset();
            motor610_set_rpm(0.0f);
            g_current_rpm = 0.0f;
            g_failsafe_active = false;
        } else if (g_bmp_ok) {
            float target_rpm = motor610_pid_update(g_current_temp, g_target_temp, dt_seconds);
            motor610_set_rpm(target_rpm);
            g_current_rpm = target_rpm;
            g_failsafe_active = false;
        } else {
            // Без датчика невозможно замкнуть PID-контур. Пока Пельтье работает,
            // безопаснее оставить максимальный обдув, чем остановить вентилятор.
            motor610_pid_reset();
            motor610_set_rpm(motor610_max_rpm);
            g_current_rpm = motor610_max_rpm;
            g_failsafe_active = true;
        }

        float diff_temp = g_current_temp - last_b_temp;
        float diff_rpm = g_current_rpm - last_b_rpm;
        
        bool changed = (diff_temp < -0.1f || diff_temp > 0.1f) ||
                       (diff_rpm < -20.0f || diff_rpm > 20.0f) ||
                       (motor610_max_rpm != last_b_max) ||
                       (g_fan_power != last_b_power) ||
                       (g_target_temp != last_b_target) ||
                       (g_bmp_ok != last_b_bmp) ||
                       (g_failsafe_active != last_b_failsafe);
                       
        TickType_t now = xTaskGetTickCount();
        bool first_broadcast = (last_broadcast == 0);
        bool interval_ok = first_broadcast ||
            ((now - last_broadcast) >= (50 / portTICK_PERIOD_MS));

        if (changed && interval_ok) {
            last_b_temp = g_current_temp;
            last_b_rpm = g_current_rpm;
            last_b_max = motor610_max_rpm;
            last_b_target = g_target_temp;
            last_b_power = g_fan_power;
            last_b_bmp = g_bmp_ok;
            last_b_failsafe = g_failsafe_active;
            last_broadcast = now;
            
            if (g_server) httpd_queue_work(g_server, broadcast_ws_data_work, NULL);
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void oled_task(void *pvParameters) {
    ssd1306_handle_t oled = (ssd1306_handle_t)pvParameters;
    while (1) {
        if (oled != NULL) {
            ssd1306_clear_screen(oled, 0x00);
            
            char t_str[32], target_str[32], r_str[32], state_str[32];
            
            // Если датчик работает - выводим температуру, иначе рисуем прочерк
            if (g_bmp_ok) {
                snprintf(t_str, sizeof(t_str), "Temp: %.1f C", g_current_temp);
            } else {
                snprintf(t_str, sizeof(t_str), "Temp: -- C");
            }
            
            // Интерфейс не падает, а продолжает рисовать остальные параметры
            snprintf(target_str, sizeof(target_str), "Target: %.1f C", g_target_temp);
            snprintf(r_str, sizeof(r_str), "Fan: %.0f", g_current_rpm);
            snprintf(state_str, sizeof(state_str), "PID: %s",
                     !g_fan_power ? "OFF" : (g_failsafe_active ? "SAFE" : "ON"));
            
            ssd1306_draw_string(oled, 0, 0, (const uint8_t *)t_str, 16, 1);
            ssd1306_draw_string(oled, 0, 16, (const uint8_t *)target_str, 16, 1);
            ssd1306_draw_string(oled, 0, 32, (const uint8_t *)r_str, 16, 1);
            ssd1306_draw_string(oled, 0, 48, (const uint8_t *)state_str, 16, 1);
            
            ssd1306_refresh_gram(oled);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
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

    xTaskCreate(bmp280_task, "bmp", 2048, NULL, 5, NULL);
    xTaskCreate(motor_task,  "mot", 2048, NULL, 5, NULL);
    xTaskCreate(oled_task,   "oled",2048, oled, 5, NULL);
}
