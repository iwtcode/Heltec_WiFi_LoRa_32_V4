#include "motor610.h"
#include <stdbool.h>

// Аппаратный (физический) максимум оборотов вентилятора при 100% ШИМ.
#define MOTOR610_PHYSICAL_MAX_RPM 45000.0f

// Текущий пользовательский лимит
float motor610_max_rpm = MOTOR610_PHYSICAL_MAX_RPM;

typedef struct {
    float integral_rpm;
    float previous_temp_c;
    bool initialized;
} motor610_pid_state_t;

static motor610_pid_state_t s_pid = { 0 };

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

void motor610_pid_reset(void)
{
    s_pid.integral_rpm = 0.0f;
    s_pid.previous_temp_c = 0.0f;
    s_pid.initialized = false;
}

float motor610_pid_update(float measured_temp_c, float target_temp_c, float dt_seconds)
{
    if (dt_seconds <= 0.0f || dt_seconds > 1.0f) {
        dt_seconds = 0.1f;
    }

    const float error_c = measured_temp_c - target_temp_c;

    // Производная берётся по измерению, а не по ошибке. Поэтому изменение
    // уставки не создаёт резкого derivative kick на выходе регулятора.
    float derivative_c_per_s = 0.0f;
    if (s_pid.initialized) {
        derivative_c_per_s = (measured_temp_c - s_pid.previous_temp_c) / dt_seconds;
    }
    s_pid.previous_temp_c = measured_temp_c;
    s_pid.initialized = true;

    const float proportional_rpm = MOTOR610_PID_KP * error_c;
    const float derivative_rpm = MOTOR610_PID_KD * derivative_c_per_s;

    // Условное интегрирование защищает от wind-up: при насыщенном выходе
    // интегратор не растёт дальше в сторону насыщения, но может уменьшаться,
    // чтобы регулятор быстро вышел из ограничения после смены температуры.
    float integral_candidate = s_pid.integral_rpm +
                               MOTOR610_PID_KI * error_c * dt_seconds;
    if (integral_candidate < 0.0f) integral_candidate = 0.0f;
    if (integral_candidate > motor610_max_rpm) integral_candidate = motor610_max_rpm;

    const float candidate_output = proportional_rpm + integral_candidate + derivative_rpm;
    const bool pushes_upper_limit = candidate_output > motor610_max_rpm && error_c > 0.0f;
    const bool pushes_lower_limit = candidate_output < 0.0f && error_c < 0.0f;
    if (!pushes_upper_limit && !pushes_lower_limit) {
        s_pid.integral_rpm = integral_candidate;
    }

    // Лимит оборотов может быть уменьшен пользователем между итерациями.
    if (s_pid.integral_rpm > motor610_max_rpm) {
        s_pid.integral_rpm = motor610_max_rpm;
    }

    float output_rpm = proportional_rpm + s_pid.integral_rpm + derivative_rpm;

    if (output_rpm < 0.0f) output_rpm = 0.0f;
    if (output_rpm > motor610_max_rpm) output_rpm = motor610_max_rpm;
    return output_rpm;
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
