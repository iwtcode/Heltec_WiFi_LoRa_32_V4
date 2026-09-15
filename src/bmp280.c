#include "bmp280.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BMP280";

#define BMP280_REG_CHIP_ID     0xD0
#define BMP280_REG_RESET       0xE0
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5
#define BMP280_REG_TEMP_MSB    0xFA
#define BMP280_REG_CALIB_T1LSB 0x88

#define BMP280_I2C_TIMEOUT_MS  1000

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
} bmp280_calib_t;

static bmp280_calib_t s_calib;
static uint8_t s_addr = 0x76;
static bool s_ready = false;

// 0x58 — BMP280, 0x56/0x57 — более ранние ревизии BMP280, 0x60 — BME280
// (на случай если вместо BMP280 будет использован pin-совместимый BME280)
static bool chip_id_valid(uint8_t id)
{
    return id == 0x58 || id == 0x56 || id == 0x57 || id == 0x60;
}

static esp_err_t write_reg(i2c_port_t i2c_num, uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(i2c_num, addr, buf, sizeof(buf),
                                       BMP280_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t read_regs(i2c_port_t i2c_num, uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(i2c_num, addr, &reg, 1, data, len,
                                         BMP280_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t try_address(i2c_port_t i2c_num, uint8_t addr)
{
    uint8_t id = 0;
    esp_err_t err = read_regs(i2c_num, addr, BMP280_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (!chip_id_valid(id)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Датчик найден по адресу 0x%02X (chip id 0x%02X)", addr, id);
    s_addr = addr;
    return ESP_OK;
}

esp_err_t bmp280_init(i2c_port_t i2c_num)
{
    s_ready = false;

    // Реальные модули BMP280 бывают на адресе 0x76 (SDO->GND) или
    // 0x77 (SDO->VCC). В симуляции Wokwi по умолчанию используется 0x76.
    if (try_address(i2c_num, 0x76) != ESP_OK &&
        try_address(i2c_num, 0x77) != ESP_OK) {
        ESP_LOGE(TAG, "BMP280 не обнаружен ни по 0x76, ни по 0x77");
        return ESP_ERR_NOT_FOUND;
    }

    // Программный сброс
    write_reg(i2c_num, s_addr, BMP280_REG_RESET, 0xB6);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Калибровочные коэффициенты температуры — 6 байт начиная с 0x88
    uint8_t calib[6] = {0};
    esp_err_t err = read_regs(i2c_num, s_addr, BMP280_REG_CALIB_T1LSB, calib, sizeof(calib));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка чтения калибровочных данных: %s", esp_err_to_name(err));
        return err;
    }
    s_calib.dig_T1 = (uint16_t)(calib[0] | (calib[1] << 8));
    s_calib.dig_T2 = (int16_t)(calib[2] | (calib[3] << 8));
    s_calib.dig_T3 = (int16_t)(calib[4] | (calib[5] << 8));

    // ctrl_meas: осреднение температуры x1, давление выключено, normal mode
    err = write_reg(i2c_num, s_addr, BMP280_REG_CTRL_MEAS, 0x23);
    if (err != ESP_OK) return err;

    // config: standby 0.5 мс, фильтр выключен
    err = write_reg(i2c_num, s_addr, BMP280_REG_CONFIG, 0x00);
    if (err != ESP_OK) return err;

    s_ready = true;
    return ESP_OK;
}

esp_err_t bmp280_read_temperature(i2c_port_t i2c_num, float *temperature_c)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[3] = {0};
    esp_err_t err = read_regs(i2c_num, s_addr, BMP280_REG_TEMP_MSB, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int32_t adc_T = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);

    // Официальная целочисленная формула компенсации из даташита Bosch BMP280
    int32_t var1, var2, t_fine;
    var1 = ((((adc_T >> 3) - ((int32_t)s_calib.dig_T1 << 1))) * ((int32_t)s_calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)s_calib.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)s_calib.dig_T1))) >> 12) * ((int32_t)s_calib.dig_T3)) >> 14;
    t_fine = var1 + var2;

    int32_t t_centi = (t_fine * 5 + 128) >> 8; // сотые доли градуса Цельсия

    *temperature_c = t_centi / 100.0f;
    return ESP_OK;
}
