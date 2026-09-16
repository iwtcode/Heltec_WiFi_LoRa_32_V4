#ifndef MOTOR610_H
#define MOTOR610_H

#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Вентилятор motor610 (coreless motor fan), см. motor610.md ---
// Пин S модуля подключаем к любому ШИМ-способному цифровому пину.
#define MOTOR610_PWM_PIN      GPIO_NUM_4
#define MOTOR610_LEDC_TIMER   LEDC_TIMER_0
#define MOTOR610_LEDC_MODE    LEDC_LOW_SPEED_MODE   // на ESP32-S3 доступен только low-speed режим LEDC
#define MOTOR610_LEDC_CHANNEL LEDC_CHANNEL_0
#define MOTOR610_LEDC_RES     LEDC_TIMER_10_BIT     // разрешение ШИМ: 0..1023
#define MOTOR610_LEDC_FREQ_HZ 5000

#define MOTOR610_MAX_DUTY     ((1 << 10) - 1) // соответствует MOTOR610_LEDC_RES (10 бит)

// Логика зависимости скорости от температуры:
//   temp <= MOTOR610_TEMP_OFF  -> вентилятор выключен (0 об/мин)
//   temp >= MOTOR610_TEMP_MAX  -> максимальная скорость (MOTOR610_MAX_RPM)
//   между ними                 -> линейная интерполяция
#define MOTOR610_TEMP_OFF  22.0f
#define MOTOR610_TEMP_MAX  30.0f
#define MOTOR610_MAX_RPM   45000.0f // паспортная скорость холостого хода модуля motor610

/**
 * Инициализация аппаратного ШИМ (LEDC) для управления вентилятором motor610.
 * Пин S модуля подключается к MOSFET-драйверу на самом модуле, поэтому
 * с цифрового пина ESP32 достаточно обычного ШИМ-сигнала небольшой мощности.
 */
void motor610_init(void);

/**
 * Пересчитывает температуру (°C) в целевую скорость вентилятора (об/мин):
 * MOTOR610_TEMP_OFF и ниже — выключен, MOTOR610_TEMP_MAX и выше — максимум,
 * между ними — линейная интерполяция.
 */
float motor610_target_rpm_for_temp(float temp_c);

/**
 * Устанавливает скважность ШИМ на пине S по целевым оборотам
 * (0..MOTOR610_MAX_RPM). Значение выходящее за диапазон обрезается.
 */
void motor610_set_rpm(float target_rpm);

#ifdef __cplusplus
}
#endif

#endif // MOTOR610_H