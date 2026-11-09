#include <stddef.h>
#include "drv8837.h"

static uint32_t duty_to_ticks(float duty)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    return (uint32_t)(duty * DRV8837_PWM_MAX_DUTY + 0.5f);
}

static void set_inputs(drv8837_t *drv, uint32_t in1_ticks, uint32_t in2_ticks)
{
    const drv8837_config_t *c = &drv->cfg;
    ledc_set_duty(c->ledc_mode, c->ch_in1, in1_ticks);
    ledc_set_duty(c->ledc_mode, c->ch_in2, in2_ticks);
    ledc_update_duty(c->ledc_mode, c->ch_in1);
    ledc_update_duty(c->ledc_mode, c->ch_in2);
}

esp_err_t drv8837_init(drv8837_t *drv, const drv8837_config_t *cfg)
{
    if (drv == NULL || cfg == NULL) return ESP_ERR_INVALID_ARG;
    drv->cfg = *cfg;
    drv->awake = false;

    // nSLEEP: сначала LOW, чтобы драйвер не стартовал с произвольными входами
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->nsleep,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(cfg->nsleep, 0);

    ledc_timer_config_t timer_conf = {
        .speed_mode = cfg->ledc_mode,
        .duty_resolution = DRV8837_PWM_RES,
        .timer_num = cfg->ledc_timer,
        .freq_hz = cfg->pwm_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) return err;

    const struct { gpio_num_t pin; ledc_channel_t ch; } inputs[] = {
        { cfg->in1, cfg->ch_in1 },
        { cfg->in2, cfg->ch_in2 },
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        ledc_channel_config_t ch_conf = {
            .gpio_num = inputs[i].pin,
            .speed_mode = cfg->ledc_mode,
            .channel = inputs[i].ch,
            .timer_sel = cfg->ledc_timer,
            .duty = 0,
            .hpoint = 0,
        };
        err = ledc_channel_config(&ch_conf);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

void drv8837_wake(drv8837_t *drv)
{
    if (drv->awake) return;
    gpio_set_level(drv->cfg.nsleep, 1);
    drv->awake = true;
}

void drv8837_sleep(drv8837_t *drv)
{
    set_inputs(drv, 0, 0);
    gpio_set_level(drv->cfg.nsleep, 0);
    drv->awake = false;
}

void drv8837_forward(drv8837_t *drv, float duty)
{
    drv8837_wake(drv);
    set_inputs(drv, duty_to_ticks(duty), 0);
}

void drv8837_reverse(drv8837_t *drv, float duty)
{
    drv8837_wake(drv);
    set_inputs(drv, 0, duty_to_ticks(duty));
}

void drv8837_coast(drv8837_t *drv)
{
    set_inputs(drv, 0, 0);
}

void drv8837_brake(drv8837_t *drv)
{
    drv8837_wake(drv);
    set_inputs(drv, DRV8837_PWM_MAX_DUTY, DRV8837_PWM_MAX_DUTY);
}
