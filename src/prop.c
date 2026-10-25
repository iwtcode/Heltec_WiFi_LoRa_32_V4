#include "prop.h"
#include <stdbool.h>

// Текущий пользовательский лимит оборотов
float prop_max_rpm = PROP_PHYSICAL_MAX_RPM;

typedef struct {
    float integral_rpm;
    float previous_temp_c;
    bool initialized;
} prop_pid_state_t;

static prop_pid_state_t s_pid = { 0 };

// Четыре входа драйвера: [0]=A.IN1, [1]=A.IN2, [2]=B.IN1, [3]=B.IN2.
// Все четыре сидят на LEDC: на «активный» вход идёт ШИМ, на второй — 0 (LOW).
static const gpio_num_t s_pins[4] = {
    PROP_A_IN1_PIN, PROP_A_IN2_PIN, PROP_B_IN1_PIN, PROP_B_IN2_PIN
};
static const ledc_channel_t s_channels[4] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3
};

#define ACTIVE_A (PROP_A_REVERSED ? 1 : 0)
#define ACTIVE_B (PROP_B_REVERSED ? 3 : 2)
#define PASSIVE_A (PROP_A_REVERSED ? 0 : 1)
#define PASSIVE_B (PROP_B_REVERSED ? 2 : 3)

#if PROP_DRIVER == PROP_DRIVER_DRV8837
static bool s_awake = false;
#endif

static void set_channel_duty(int idx, uint32_t duty)
{
    ledc_set_duty(PROP_LEDC_MODE, s_channels[idx], duty);
    ledc_update_duty(PROP_LEDC_MODE, s_channels[idx]);
}

// Только DRV8837: nSLEEP HIGH — драйвер работает, LOW — сон (120 нА).
static void set_awake(bool awake)
{
#if PROP_DRIVER == PROP_DRIVER_DRV8837
    if (awake != s_awake) {
        gpio_set_level(PROP_NSLEEP_PIN, awake ? 1 : 0);
        s_awake = awake;
    }
#else
    (void)awake;
#endif
}

void prop_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = PROP_LEDC_MODE,
        .duty_resolution = PROP_LEDC_RES,
        .timer_num = PROP_LEDC_TIMER,
        .freq_hz = PROP_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t channel_conf = {
            .gpio_num = s_pins[i],
            .speed_mode = PROP_LEDC_MODE,
            .channel = s_channels[i],
            .timer_sel = PROP_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&channel_conf);
    }

#if PROP_DRIVER == PROP_DRIVER_DRV8837
    gpio_reset_pin(PROP_NSLEEP_PIN);
    gpio_set_direction(PROP_NSLEEP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PROP_NSLEEP_PIN, 0);   // стартуем в режиме сна
    s_awake = false;
#endif
}

void prop_pid_reset(void)
{
    s_pid.integral_rpm = 0.0f;
    s_pid.previous_temp_c = 0.0f;
    s_pid.initialized = false;
}

float prop_pid_update(float measured_temp_c, float target_temp_c, float dt_seconds)
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

    const float proportional_rpm = PROP_PID_KP * error_c;
    const float derivative_rpm = PROP_PID_KD * derivative_c_per_s;

    // Условное интегрирование защищает от wind-up: при насыщенном выходе
    // интегратор не растёт дальше в сторону насыщения, но может уменьшаться,
    // чтобы регулятор быстро вышел из ограничения после смены температуры.
    float integral_candidate = s_pid.integral_rpm +
                               PROP_PID_KI * error_c * dt_seconds;
    if (integral_candidate < 0.0f) integral_candidate = 0.0f;
    if (integral_candidate > prop_max_rpm) integral_candidate = prop_max_rpm;

    const float candidate_output = proportional_rpm + integral_candidate + derivative_rpm;
    const bool pushes_upper_limit = candidate_output > prop_max_rpm && error_c > 0.0f;
    const bool pushes_lower_limit = candidate_output < 0.0f && error_c < 0.0f;
    if (!pushes_upper_limit && !pushes_lower_limit) {
        s_pid.integral_rpm = integral_candidate;
    }

    // Лимит оборотов может быть уменьшен пользователем между итерациями.
    if (s_pid.integral_rpm > prop_max_rpm) {
        s_pid.integral_rpm = prop_max_rpm;
    }

    float output_rpm = proportional_rpm + s_pid.integral_rpm + derivative_rpm;

    if (output_rpm < 0.0f) output_rpm = 0.0f;
    if (output_rpm > prop_max_rpm) output_rpm = prop_max_rpm;
    return output_rpm;
}

float prop_set_rpm(float target_rpm)
{
    // Ограничиваем пользовательским лимитом и физическим максимумом
    if (target_rpm < 0.0f) target_rpm = 0.0f;
    if (target_rpm > prop_max_rpm) target_rpm = prop_max_rpm;
    if (target_rpm > PROP_PHYSICAL_MAX_RPM) target_rpm = PROP_PHYSICAL_MAX_RPM;

    // Микромоторы не стартуют при очень малом напряжении:
    // совсем малая команда = выключено, иначе не ниже минимума запуска.
    if (target_rpm < PROP_OFF_BELOW_RPM) {
        target_rpm = 0.0f;
    } else if (target_rpm < PROP_MIN_RUN_RPM) {
        target_rpm = PROP_MIN_RUN_RPM;
    }

    // Скважность считаем от ФИЗИЧЕСКОГО максимума при текущем питании
    // и дополнительно режем так, чтобы на моторе было не больше 4,2 В.
    float fraction = target_rpm / PROP_PHYSICAL_MAX_RPM * PROP_DUTY_CAP;
    if (fraction > PROP_DUTY_CAP) fraction = PROP_DUTY_CAP;
    if (fraction > 1.0f) fraction = 1.0f;

    uint32_t duty = (uint32_t)(fraction * PROP_MAX_DUTY + 0.5f);

    if (duty > 0) {
        set_awake(true);
    }

    set_channel_duty(ACTIVE_A, duty);
    set_channel_duty(PASSIVE_A, 0);
    set_channel_duty(ACTIVE_B, duty);
    set_channel_duty(PASSIVE_B, 0);

    if (duty == 0) {
        set_awake(false);
    }

    return target_rpm;
}
