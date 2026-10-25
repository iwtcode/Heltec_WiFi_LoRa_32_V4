#ifndef PROP_H
#define PROP_H

#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Два микромотора 6x15 мм с разнонаправленными пропеллерами (см. micro_motors.md).
 * Моторы подключаются через H-мост-драйвер. Поддерживаются два варианта
 * (выбирается одной строкой PROP_DRIVER):
 *
 *   PROP_DRIVER_MX1508   один модуль MX1508 на оба мотора (канал A = мотор 1,
 *                        канал B = мотор 2); входы IN1..IN4.
 *   PROP_DRIVER_DRV8837  два модуля DRV8837 (по одному на мотор); у каждого
 *                        свои IN1/IN2, а выводы nSLEEP обоих модулей соединены
 *                        вместе и подключены к PROP_NSLEEP_PIN.
 *
 * Схема управления одинакова для обоих драйверов: ШИМ подаётся на один из
 * двух входов мотора, второй вход удерживается в LOW. Меняя, на какой вход
 * идёт ШИМ, меняют направление вращения (PROP_x_REVERSED).
 */
#define PROP_DRIVER_MX1508   1
#define PROP_DRIVER_DRV8837  2
#define PROP_DRIVER          PROP_DRIVER_MX1508

// Мотор 1 (MX1508: IN1/IN2, DRV8837 №1: IN1/IN2)
#define PROP_A_IN1_PIN   GPIO_NUM_4
#define PROP_A_IN2_PIN   GPIO_NUM_5
// Мотор 2 (MX1508: IN3/IN4, DRV8837 №2: IN1/IN2)
#define PROP_B_IN1_PIN   GPIO_NUM_6
#define PROP_B_IN2_PIN   GPIO_NUM_7
// Только для DRV8837: общий вывод nSLEEP (HIGH = работа, LOW = сон, 120 нА)
#define PROP_NSLEEP_PIN  GPIO_NUM_2

// Если пропеллер дует «не в ту сторону» — поменяйте флаг (или поменяйте
// местами провода мотора). 0: ШИМ на IN1 (IN3), 1: ШИМ на IN2 (IN4).
#define PROP_A_REVERSED  0
#define PROP_B_REVERSED  0

// --- Параметры моторов из спецификации ---
// 46 500 об/мин достигаются при максимальном напряжении мотора 4,2 В.
#define PROP_RATED_V         4.2f
#define PROP_RPM_AT_RATED_V  46500.0f

// Напряжение, которым питаются моторы (VM у DRV8837 / VCC у MX1508):
//   1S Li-ion/LiPo — 4.2f (заряжен) .. 3.7f, повербанк 5 В — 5.0f.
// Скважность ШИМ автоматически ограничивается так, чтобы действующее
// напряжение на моторе не превышало 4,2 В.
#define PROP_SUPPLY_V        4.2f

#define PROP_DUTY_CAP        ((PROP_SUPPLY_V) > (PROP_RATED_V) ? ((PROP_RATED_V) / (PROP_SUPPLY_V)) : 1.0f)
#define PROP_PHYSICAL_MAX_RPM \
    ((PROP_RPM_AT_RATED_V) * ((PROP_SUPPLY_V) < (PROP_RATED_V) ? ((PROP_SUPPLY_V) / (PROP_RATED_V)) : 1.0f))

// Микромоторы не стартуют при очень малом напряжении. Любая ненулевая
// команда ниже PROP_MIN_RUN_RPM поднимается до этого значения, а команда
// ниже PROP_OFF_BELOW_RPM выключает моторы полностью.
#define PROP_MIN_RUN_RPM     4000.0f
#define PROP_OFF_BELOW_RPM   500.0f

// ШИМ: 20 кГц — вне слышимого диапазона, у безжелезных моторов малая индуктивность.
#define PROP_LEDC_TIMER      LEDC_TIMER_0
#define PROP_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define PROP_LEDC_RES        LEDC_TIMER_10_BIT
#define PROP_LEDC_FREQ_HZ    20000
#define PROP_MAX_DUTY        ((1 << 10) - 1)

// Пользовательский верхний предел оборотов, настраиваемый через Wi-Fi.
extern float prop_max_rpm;

// Коэффициенты PID-регулятора. Выход регулятора задаётся в об/мин.
#define PROP_PID_KP 7000.0f
#define PROP_PID_KI 600.0f
#define PROP_PID_KD 1500.0f

/** Инициализация ШИМ (LEDC) на четырёх входах драйвера и вывода nSLEEP. */
void prop_init(void);

/** Сбрасывает накопленное состояние PID-регулятора. */
void prop_pid_reset(void);

/**
 * Вычисляет целевую скорость пропеллеров PID-регулятором.
 * Ошибка положительна, когда элемент Пельтье горячее заданной температуры.
 */
float prop_pid_update(float measured_temp_c, float target_temp_c, float dt_seconds);

/**
 * Задаёт обороты обоих моторов (одинаковые). Возвращает фактически
 * установленные обороты — с учётом лимита, минимума запуска и отключения.
 */
float prop_set_rpm(float target_rpm);

#ifdef __cplusplus
}
#endif

#endif // PROP_H
