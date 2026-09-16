#include "motor610.h"

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
        return MOTOR610_MAX_RPM;
    }
    float fraction = (temp_c - MOTOR610_TEMP_OFF) / (MOTOR610_TEMP_MAX - MOTOR610_TEMP_OFF);
    return MOTOR610_MAX_RPM * fraction;
}

void motor610_set_rpm(float target_rpm)
{
    if (target_rpm < 0.0f) target_rpm = 0.0f;
    if (target_rpm > MOTOR610_MAX_RPM) target_rpm = MOTOR610_MAX_RPM;

    float fraction = target_rpm / MOTOR610_MAX_RPM;
    uint32_t duty = (uint32_t)(fraction * MOTOR610_MAX_DUTY + 0.5f);

    ledc_set_duty(MOTOR610_LEDC_MODE, MOTOR610_LEDC_CHANNEL, duty);
    ledc_update_duty(MOTOR610_LEDC_MODE, MOTOR610_LEDC_CHANNEL);
}