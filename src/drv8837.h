#ifndef DRV8837_H
#define DRV8837_H

/*
 * Драйвер H-моста TI DRV8837 (см. specs.md): VM 0..11 В, логика 1.8..7 В,
 * до 1.8 А, интерфейс IN1/IN2 (PWM) + nSLEEP.
 *
 * Таблица состояний:
 *   nSLEEP IN1 IN2 | OUT1 OUT2
 *     0     x   x  |  Z    Z    сон (120 нА)
 *     1     0   0  |  Z    Z    выбег (coast)
 *     1     PWM 0  |  вперёд (скважность = мощность)
 *     1     0   PWM|  назад
 *     1     1   1  |  L    L    торможение (brake)
 *
 * ВАЖНО: нельзя подавать VM без питания логики VCC (см. specs.md).
 * Модуль ничего не знает о конкретном моторе — только о H-мосте.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRV8837_PWM_RES        LEDC_TIMER_10_BIT
#define DRV8837_PWM_MAX_DUTY   ((1U << 10) - 1)

typedef struct {
    gpio_num_t     in1;        // вход IN1 (PWM)
    gpio_num_t     in2;        // вход IN2 (PWM)
    gpio_num_t     nsleep;     // nSLEEP: HIGH = активен
    ledc_mode_t    ledc_mode;
    ledc_timer_t   ledc_timer;
    ledc_channel_t ch_in1;
    ledc_channel_t ch_in2;
    uint32_t       pwm_freq_hz;   // до 250 кГц по даташиту
} drv8837_config_t;

typedef struct {
    drv8837_config_t cfg;
    bool awake;
} drv8837_t;

/** Настраивает LEDC и nSLEEP; драйвер остаётся в режиме сна, выходы в Z. */
esp_err_t drv8837_init(drv8837_t *drv, const drv8837_config_t *cfg);

/** Выводит из сна (nSLEEP = HIGH). */
void drv8837_wake(drv8837_t *drv);

/** Усыпляет драйвер (выходы в Z, ток сна ~120 нА). */
void drv8837_sleep(drv8837_t *drv);

/** Вращение вперёд, duty 0.0..1.0 (значение за пределами обрезается). */
void drv8837_forward(drv8837_t *drv, float duty);

/** Вращение назад, duty 0.0..1.0. */
void drv8837_reverse(drv8837_t *drv, float duty);

/** Выбег: оба выхода в Z. */
void drv8837_coast(drv8837_t *drv);

/** Торможение: оба низа открыты. */
void drv8837_brake(drv8837_t *drv);

#ifdef __cplusplus
}
#endif

#endif // DRV8837_H
