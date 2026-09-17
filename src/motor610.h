#ifndef MOTOR610_H
#define MOTOR610_H

#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Вентилятор motor610 (coreless motor fan), см. motor610.md ---
#define MOTOR610_PWM_PIN      GPIO_NUM_4
#define MOTOR610_LEDC_TIMER   LEDC_TIMER_0
#define MOTOR610_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define MOTOR610_LEDC_CHANNEL LEDC_CHANNEL_0
#define MOTOR610_LEDC_RES     LEDC_TIMER_10_BIT
#define MOTOR610_LEDC_FREQ_HZ 5000

#define MOTOR610_MAX_DUTY     ((1 << 10) - 1)

// Логика зависимости скорости от температуры:
extern float motor610_temp_min;
extern float motor610_temp_max;

// Теперь это не макрос, а глобальная переменная, которую можно менять через Wi-Fi
extern float motor610_max_rpm;

/**
 * Инициализация аппаратного ШИМ (LEDC) для управления вентилятором motor610.
 */
void motor610_init(void);

/**
 * Пересчитывает температуру (°C) в целевую скорость вентилятора (об/мин).
 */
float motor610_target_rpm_for_temp(float temp_c);

/**
 * Устанавливает скважность ШИМ на пине S по целевым оборотам.
 */
void motor610_set_rpm(float target_rpm);

#ifdef __cplusplus
}
#endif

#endif // MOTOR610_H