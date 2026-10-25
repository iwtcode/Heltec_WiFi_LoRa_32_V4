// Wokwi Custom Chip — симуляционная модель модуля драйвера MX1508 с двумя
// микромоторами 6x15 мм и разнонаправленными пропеллерами (см. micro_motors.md).
// Docs: https://docs.wokwi.com/chips-api/getting-started
//
// Пины модуля (как на реальной плате MX1508):
//   VCC      — питание модуля (в симуляции только проверяется, что оно есть)
//   GND      — земля
//   IN1, IN2 — вход канала A (мотор 1, левый пропеллер)
//   IN3, IN4 — вход канала B (мотор 2, правый пропеллер)
//
// Логика H-моста как у реального MX1508:
//   IN1 = ШИМ, IN2 = LOW  -> мотор крутится вперёд (скорость ~ скважности)
//   IN1 = LOW, IN2 = ШИМ  -> мотор крутится назад
//   оба LOW               -> выбег, оба HIGH -> торможение (скорость 0)
// Эффективное управляющее воздействие канала = duty(IN_a) - duty(IN_b).
//
// Скважность каждого входа измеряется накоплением времени HIGH/LOW за
// окно кадра анимации — так же, как это сделано в предыдущей модели вентилятора.
//
// Напряжение питания моторов задаётся слайдером "Motor supply (V)" на самом
// чипе (кликните по нему во время симуляции). Обороты считаются по закону
//   rpm = 46500 * (|drive| * Vsupply) / 4.2 В,
// то есть 46 500 об/мин достигаются при 4,2 В на моторе (максимум по
// спецификации). Если действующее напряжение выше 4,2 В, ступицы пропеллеров
// краснеют — признак перенапряжения (прошивка этого не допускает: она сама
// ограничивает скважность, см. PROP_SUPPLY_V в prop.h).
//
// Левый пропеллер вращается по часовой стрелке, правый — против: это пара
// «разнонаправленных» пропеллеров из комплекта. Если скважность отрицательна
// (мотор крутится в обратную сторону), лопасти окрашиваются в синий.

#include "wokwi-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

#define RATED_V 4.2f
#define RPM_AT_RATED_V 46500.0f
#define DEFAULT_SUPPLY_V 4.2f

// Постоянная времени сглаживания (инерция ротора), мс (атрибут smoothingMs).
#define DEFAULT_SMOOTHING_MS 250.0f

#define ANIM_INTERVAL_US 16667 // 60 кадров/с

// Визуальное замедление вращения (только для анимации).
#define VISUAL_SPIN_SCALE 0.005f

typedef struct {
  pin_t pin_vcc;
  pin_t pin_gnd;
  pin_t pin_in[4];          // IN1..IN4

  uint32_t level[4];        // последний известный уровень входа
  uint64_t t_level_start[4];
  uint64_t acc_high_ns[4];
  uint64_t acc_low_ns[4];

  float drive_smooth[2];    // сглаженное знаковое управление каналов A и B, -1..1
  float angle[2];           // углы поворота пропеллеров, радианы
  float over_voltage[2];    // 1.0 если действующее напряжение > 4.2 В

  buffer_t fb;
  uint32_t fb_w;
  uint32_t fb_h;
  uint8_t *img;

  uint64_t t_frame_last;
  uint32_t attr_supply;
  float supply_v;
  float smoothing_s;
  timer_t anim_timer;
} chip_state_t;

static void on_pin_change(void *user_data, pin_t pin, uint32_t value);
static void on_anim_timer(void *user_data);
static void render_frame(chip_state_t *chip);
static inline void set_px(chip_state_t *chip, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float angle_wrap(float a) {
  while (a >= 2.0f * PI_F) a -= 2.0f * PI_F;
  while (a < 0.0f) a += 2.0f * PI_F;
  return a;
}
static inline float angle_diff(float a, float b) {
  float d = fmodf(a - b, 2.0f * PI_F);
  if (d > PI_F) d -= 2.0f * PI_F;
  if (d < -PI_F) d += 2.0f * PI_F;
  return d;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  memset(chip, 0, sizeof(chip_state_t));

  chip->pin_vcc = pin_init("VCC", INPUT);
  chip->pin_gnd = pin_init("GND", INPUT);
  chip->pin_in[0] = pin_init("IN1", INPUT);
  chip->pin_in[1] = pin_init("IN2", INPUT);
  chip->pin_in[2] = pin_init("IN3", INPUT);
  chip->pin_in[3] = pin_init("IN4", INPUT);

  chip->attr_supply = attr_init_float("supplyV", DEFAULT_SUPPLY_V);
  chip->supply_v = attr_read_float(chip->attr_supply);
  if (chip->supply_v < 0.0f) chip->supply_v = 0.0f;

  float smoothing_ms = attr_read_float(attr_init_float("smoothingMs", DEFAULT_SMOOTHING_MS));
  if (smoothing_ms < 0.0f) smoothing_ms = 0.0f;
  chip->smoothing_s = smoothing_ms / 1000.0f;

  uint64_t now = get_sim_nanos();
  chip->t_frame_last = now;

  const pin_watch_config_t watch_config = {
    .edge = BOTH,
    .pin_change = on_pin_change,
    .user_data = chip,
  };
  for (int i = 0; i < 4; i++) {
    chip->level[i] = pin_read(chip->pin_in[i]);
    chip->t_level_start[i] = now;
    pin_watch(chip->pin_in[i], &watch_config);
  }

  chip->fb = framebuffer_init(&chip->fb_w, &chip->fb_h);
  chip->img = malloc((size_t)chip->fb_w * chip->fb_h * 4);

  const timer_config_t anim_timer_config = {
    .callback = on_anim_timer,
    .user_data = chip,
  };
  chip->anim_timer = timer_init(&anim_timer_config);
  timer_start(chip->anim_timer, ANIM_INTERVAL_US, true);

  render_frame(chip);

  printf("MX1508 + 2 микромотора (симуляция): питание моторов %.1f В, сглаживание %.0f мс\n",
         chip->supply_v, smoothing_ms);
}

static void on_pin_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *chip = (chip_state_t *)user_data;
  uint64_t now = get_sim_nanos();

  for (int i = 0; i < 4; i++) {
    if (chip->pin_in[i] != pin) continue;
    uint64_t dt = now - chip->t_level_start[i];
    if (chip->level[i] == HIGH) {
      chip->acc_high_ns[i] += dt;
    } else {
      chip->acc_low_ns[i] += dt;
    }
    chip->level[i] = value;
    chip->t_level_start[i] = now;
    return;
  }
}

static void on_anim_timer(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  uint64_t now = get_sim_nanos();

  // Скважность каждого из четырёх входов за прошедшее окно.
  float duty[4];
  for (int i = 0; i < 4; i++) {
    uint64_t tail = now - chip->t_level_start[i];
    uint64_t high_ns = chip->acc_high_ns[i] + (chip->level[i] == HIGH ? tail : 0);
    uint64_t low_ns  = chip->acc_low_ns[i]  + (chip->level[i] == LOW  ? tail : 0);
    uint64_t total_ns = high_ns + low_ns;
    duty[i] = (total_ns > 0) ? (float)high_ns / (float)total_ns
                             : (chip->level[i] == HIGH ? 1.0f : 0.0f);
    if (duty[i] < 0.0f) duty[i] = 0.0f;
    if (duty[i] > 1.0f) duty[i] = 1.0f;
    chip->acc_high_ns[i] = 0;
    chip->acc_low_ns[i] = 0;
    chip->t_level_start[i] = now;
  }

  float dt_s = (float)(now - chip->t_frame_last) / 1e9f;
  chip->t_frame_last = now;
  if (dt_s < 0.0f || dt_s > 0.5f) dt_s = 0.0f;

  // Слайдер напряжения питания моторов; нет питания на VCC — моторы стоят.
  float supply = attr_read_float(chip->attr_supply);
  if (supply < 0.0f) supply = 0.0f;
  chip->supply_v = supply;
  const bool powered = (pin_read(chip->pin_vcc) == HIGH);

  for (int ch = 0; ch < 2; ch++) {
    float drive_raw = duty[ch * 2] - duty[ch * 2 + 1];
    if (!powered) drive_raw = 0.0f;

    if (chip->smoothing_s > 0.0f && dt_s > 0.0f) {
      float alpha = dt_s / (chip->smoothing_s + dt_s);
      chip->drive_smooth[ch] += alpha * (drive_raw - chip->drive_smooth[ch]);
    } else {
      chip->drive_smooth[ch] = drive_raw;
    }

    float v_eff = fabsf(chip->drive_smooth[ch]) * chip->supply_v;
    chip->over_voltage[ch] = (v_eff > RATED_V + 0.05f) ? 1.0f : 0.0f;

    // Знаковые обороты (отрицательные — вращение «против» штатного направления).
    float rpm = RPM_AT_RATED_V * (chip->drive_smooth[ch] * chip->supply_v) / RATED_V;
    float angular_speed = rpm * 2.0f * PI_F / 60.0f * VISUAL_SPIN_SCALE; // рад/с
    // Левый пропеллер (канал A) — по часовой, правый (канал B) — против.
    float handed = (ch == 0) ? 1.0f : -1.0f;
    chip->angle[ch] = angle_wrap(chip->angle[ch] + handed * angular_speed * dt_s);
  }

  render_frame(chip);
}

static inline void set_px(chip_state_t *chip, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (x < 0 || y < 0 || (uint32_t)x >= chip->fb_w || (uint32_t)y >= chip->fb_h) {
    return;
  }
  size_t off = ((size_t)y * chip->fb_w + (size_t)x) * 4;
  chip->img[off + 0] = r;
  chip->img[off + 1] = g;
  chip->img[off + 2] = b;
  chip->img[off + 3] = a;
}

static void render_frame(chip_state_t *chip) {
  if (!chip->img) return;

  const int w = (int)chip->fb_w;
  const int h = (int)chip->fb_h;
  const float half_w = (float)w / 2.0f;
  const float cy = (float)h / 2.0f;
  const float cxs[2] = { half_w / 2.0f, half_w + half_w / 2.0f };
  const float min_dim = (half_w < (float)h ? half_w : (float)h);
  const float r_outer = min_dim / 2.0f - 2.0f;
  const float r_frame_inner = r_outer * 0.88f;
  const float hub_r = r_outer * 0.16f;
  const float blade_r = r_outer * 0.80f;
  const float blade_half_width = 0.34f;
  const float blade_curve_mag = 0.55f;
  const int blades = 3;

  // Цвет лопастей каждого пропеллера: серый -> оранжевый (вперёд) или синий (назад).
  uint8_t br[2], bg[2], bb[2];
  for (int ch = 0; ch < 2; ch++) {
    float mag = fabsf(chip->drive_smooth[ch]);
    if (mag > 1.0f) mag = 1.0f;
    if (chip->drive_smooth[ch] >= 0.0f) {
      br[ch] = (uint8_t)lerp(130.0f, 255.0f, mag);
      bg[ch] = (uint8_t)lerp(130.0f, 150.0f, mag);
      bb[ch] = (uint8_t)lerp(130.0f, 40.0f, mag);
    } else {
      br[ch] = (uint8_t)lerp(130.0f, 60.0f, mag);
      bg[ch] = (uint8_t)lerp(130.0f, 140.0f, mag);
      bb[ch] = (uint8_t)lerp(130.0f, 255.0f, mag);
    }
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int ch = ((float)x < half_w) ? 0 : 1;
      float fx = (float)x - cxs[ch];
      float fy = (float)y - cy;
      float r = sqrtf(fx * fx + fy * fy);

      if (r > r_outer) {
        set_px(chip, x, y, 0, 0, 0, 0);
        continue;
      }

      float theta = atan2f(fy, fx);
      uint8_t r8, g8, b8;

      if (r > r_frame_inner) {
        r8 = 205; g8 = 205; b8 = 210;      // обод
      } else {
        r8 = 25; g8 = 25; b8 = 28;         // фон за пропеллером
      }

      if (r >= hub_r && r <= blade_r) {
        float frac = (r - hub_r) / (blade_r - hub_r);
        float hw = blade_half_width * (1.0f - 0.35f * frac);
        // Правый пропеллер зеркален левому — «разнонаправленный».
        float curve = (ch == 0 ? 1.0f : -1.0f) * blade_curve_mag * frac;
        for (int k = 0; k < blades; k++) {
          float base_angle = (float)k * (2.0f * PI_F / (float)blades);
          float center_angle = angle_wrap(chip->angle[ch] + base_angle + curve);
          float d = angle_diff(theta, center_angle);
          if (fabsf(d) < hw) {
            r8 = br[ch]; g8 = bg[ch]; b8 = bb[ch];
            break;
          }
        }
      }

      if (r <= hub_r) {
        float shade = 1.0f - (r / hub_r) * 0.4f;
        if (chip->over_voltage[ch] > 0.5f) {
          r8 = (uint8_t)(210.0f * shade);
          g8 = (uint8_t)(40.0f * shade);
          b8 = (uint8_t)(40.0f * shade);
        } else {
          r8 = (uint8_t)(45.0f * shade);
          g8 = (uint8_t)(45.0f * shade);
          b8 = (uint8_t)(50.0f * shade);
        }
      }

      set_px(chip, x, y, r8, g8, b8, 255);
    }
  }

  buffer_write(chip->fb, 0, chip->img, chip->fb_w * chip->fb_h * 4);
}
