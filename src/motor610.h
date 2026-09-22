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

// Пользовательский верхний предел оборотов, настраиваемый через Wi-Fi.
extern float motor610_max_rpm;

// Коэффициенты PID-регулятора. Выход регулятора задаётся в об/мин.
#define MOTOR610_PID_KP 7000.0f
#define MOTOR610_PID_KI 600.0f
#define MOTOR610_PID_KD 1500.0f

/**
 * Инициализация аппаратного ШИМ (LEDC) для управления вентилятором motor610.
 */
void motor610_init(void);

/**
 * Сбрасывает накопленное состояние PID-регулятора.
 */
void motor610_pid_reset(void);

/**
 * Вычисляет целевую скорость вентилятора PID-регулятором.
 * Ошибка положительна, когда элемент Пельтье горячее заданной температуры.
 *
 * @param measured_temp_c измеренная температура элемента Пельтье, °C
 * @param target_temp_c   целевая температура, °C
 * @param dt_seconds      время с предыдущего вычисления, с
 */
float motor610_pid_update(float measured_temp_c, float target_temp_c, float dt_seconds);

/**
 * Устанавливает скважность ШИМ на пине S по целевым оборотам.
 */
void motor610_set_rpm(float target_rpm);

#ifdef __cplusplus
}
#endif

#endif // MOTOR610_H
