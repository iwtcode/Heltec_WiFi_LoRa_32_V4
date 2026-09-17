#include "motor610.h"

// Аппаратный (физический) максимум оборотов вентилятора при 100% ШИМ.
#define MOTOR610_PHYSICAL_MAX_RPM 45000.0f

// Текущий пользовательский лимит
float motor610_max_rpm = MOTOR610_PHYSICAL_MAX_RPM;

void motor610_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = MOTOR610_LEDC_MODE,
        .duty_resolution = MOTOR610_LEDC_RES,
        .timer_num = MOTOR610_LEDC_TIMER,
        .freq_hz = MOTOR610_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num = MOTOR610_PWM_PIN,
        .speed_mode = MOTOR610_LEDC_MODE,
        .channel = MOTOR610_LEDC_CHANNEL,
        .timer_sel = MOTOR610_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel_conf);
}

float motor610_target_rpm_for_temp(float temp_c)
{
    if (temp_c <= MOTOR610_TEMP_OFF) {
        return 0.0f;
    }
    if (temp_c >= MOTOR610_TEMP_MAX) {
        return motor610_max_rpm;
    }
    float fraction = (temp_c - MOTOR610_TEMP_OFF) / (MOTOR610_TEMP_MAX - MOTOR610_TEMP_OFF);
    return motor610_max_rpm * fraction;
}

void motor610_set_rpm(float target_rpm)
{
    // Ограничиваем желаемые обороты текущим пользовательским лимитом
    if (target_rpm < 0.0f) target_rpm = 0.0f;
    if (target_rpm > motor610_max_rpm) target_rpm = motor610_max_rpm;

    // ВАЖНО: скважность (мощность) ШИМ мы всегда считаем от ФИЗИЧЕСКИХ возможностей мотора!
    // Если мотор максимум выдает 45000, а мы хотим 22500, то ШИМ должен быть 50% (22500 / 45000).
    float fraction = target_rpm / MOTOR610_PHYSICAL_MAX_RPM;
    
    // Защита от превышения 100% скважности на всякий случай
    if (fraction > 1.0f) fraction = 1.0f;

    uint32_t duty = (uint32_t)(fraction * MOTOR610_MAX_DUTY + 0.5f);

    ledc_set_duty(MOTOR610_LEDC_MODE, MOTOR610_LEDC_CHANNEL, duty);
    ledc_update_duty(MOTOR610_LEDC_MODE, MOTOR610_LEDC_CHANNEL);
}