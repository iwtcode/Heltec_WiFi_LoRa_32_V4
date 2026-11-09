#include "micro_motor.h"
#include "drv8837.h"

// Ограничение по документации: мотор нельзя кормить больше 4.2 В
#if MICRO_MOTOR_SUPPLY_MV > 4200
#define SUPPLY_OVER_LIMIT 1
#else
#define SUPPLY_OVER_LIMIT 0
#endif

static drv8837_t s_drv;

float micro_motor_temp_min = 22.0f;
float micro_motor_temp_max = 30.0f;
float micro_motor_max_rpm  = MICRO_MOTOR_RATED_RPM * MICRO_MOTOR_SUPPLY_MV / MICRO_MOTOR_MAX_VOLTAGE_MV;

// Эффективное напряжение на моторе не должно превышать паспортный максимум
static float max_effective_mv(void)
{
    return SUPPLY_OVER_LIMIT ? (float)MICRO_MOTOR_MAX_VOLTAGE_MV : (float)MICRO_MOTOR_SUPPLY_MV;
}

float micro_motor_physical_max_rpm(void)
{
    // Холостые обороты примерно пропорциональны напряжению
    return MICRO_MOTOR_RATED_RPM * max_effective_mv() / MICRO_MOTOR_MAX_VOLTAGE_MV;
}

void micro_motor_init(void)
{
    const drv8837_config_t cfg = {
        .in1 = DRV8837_IN1_PIN,
        .in2 = DRV8837_IN2_PIN,
        .nsleep = DRV8837_NSLEEP_PIN,
        .ledc_mode = LEDC_LOW_SPEED_MODE,
        .ledc_timer = LEDC_TIMER_0,
        .ch_in1 = LEDC_CHANNEL_0,
        .ch_in2 = LEDC_CHANNEL_1,
        .pwm_freq_hz = 20000,   // выше слышимого диапазона
    };
    drv8837_init(&s_drv, &cfg);
    micro_motor_max_rpm = micro_motor_physical_max_rpm();
}

float micro_motor_target_rpm_for_temp(float temp_c)
{
    if (temp_c <= micro_motor_temp_min) {
        return 0.0f;
    }
    if (temp_c >= micro_motor_temp_max) {
        return micro_motor_max_rpm;
    }
    float fraction = (temp_c - micro_motor_temp_min) / (micro_motor_temp_max - micro_motor_temp_min);
    return micro_motor_max_rpm * fraction;
}

void micro_motor_set_rpm(float target_rpm)
{
    float phys_max = micro_motor_physical_max_rpm();
    if (target_rpm < 0.0f) target_rpm = 0.0f;
    if (target_rpm > micro_motor_max_rpm) target_rpm = micro_motor_max_rpm;
    if (target_rpm > phys_max) target_rpm = phys_max;

    if (target_rpm <= 0.0f) {
        drv8837_coast(&s_drv);
        drv8837_sleep(&s_drv);
        return;
    }

    // Среднее напряжение на моторе = VM * duty; обороты ~ напряжению.
    // duty = (rpm / RATED_RPM) * (4.2 В / VM)
    float duty = (target_rpm / MICRO_MOTOR_RATED_RPM) *
                 ((float)MICRO_MOTOR_MAX_VOLTAGE_MV / (float)MICRO_MOTOR_SUPPLY_MV);
    drv8837_forward(&s_drv, duty);
}
