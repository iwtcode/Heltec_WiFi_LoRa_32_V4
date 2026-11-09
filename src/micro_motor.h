#ifndef MICRO_MOTOR_H
#define MICRO_MOTOR_H

/*
 * Микродвигатель с пропеллером (см. specs.md), управляется через DRV8837.
 * Слои: drv8837 (H-мост) <- micro_motor (профиль мотора, обороты<->ШИМ,
 * кривая температура->обороты) <- main.c (задачи, Wi-Fi, OLED).
 */

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Паспортные данные мотора (specs.md) ---
#define MICRO_MOTOR_RATED_RPM        46500.0f  // при максимальном напряжении
#define MICRO_MOTOR_MAX_VOLTAGE_MV   4200      // выше нельзя
#define MICRO_MOTOR_NOMINAL_MV       3700      // ~0.1 А, тяга 11-12 г (4.2 В: 14.8 г)

// Напряжение на выводе VM драйвера. По умолчанию — одна Li-Ion банка (3.7 В).
// Переопределяется build_flags: -DMICRO_MOTOR_SUPPLY_MV=4200
#ifndef MICRO_MOTOR_SUPPLY_MV
#define MICRO_MOTOR_SUPPLY_MV        MICRO_MOTOR_NOMINAL_MV
#endif

// Логика зависимости скорости от температуры
extern float micro_motor_temp_min;
extern float micro_motor_temp_max;

// Пользовательский лимит оборотов (меняется через Wi-Fi дашборд)
extern float micro_motor_max_rpm;

/** Инициализация драйвера DRV8837 (пины — в board_config.h). */
void micro_motor_init(void);

/** Максимум оборотов при текущем напряжении VM (с учётом лимита 4.2 В). */
float micro_motor_physical_max_rpm(void);

/** Температура (°C) -> целевые обороты (об/мин). */
float micro_motor_target_rpm_for_temp(float temp_c);

/** Задаёт обороты; 0 усыпляет драйвер. */
void micro_motor_set_rpm(float target_rpm);

#ifdef __cplusplus
}
#endif

#endif // MICRO_MOTOR_H
