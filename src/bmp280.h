#ifndef BMP280_H
#define BMP280_H

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Инициализация датчика BMP280 на указанной шине I2C.
 * Функция сама определяет адрес датчика (0x76 или 0x77) и читает
 * калибровочные коэффициенты, необходимые для расчёта температуры.
 *
 * Работает одинаково как с реальным датчиком BMP280, так и с его
 * симуляционной моделью (custom chip "chip-bmp280") в Wokwi.
 */
esp_err_t bmp280_init(i2c_port_t i2c_num);

/**
 * Считывает текущую температуру с датчика BMP280 (в градусах Цельсия).
 * Перед вызовом необходимо успешно выполнить bmp280_init().
 */
esp_err_t bmp280_read_temperature(i2c_port_t i2c_num, float *temperature_c);

#ifdef __cplusplus
}
#endif

#endif // BMP280_H
