// Wokwi Custom Chip — симуляционная модель бесколлекторного вентилятора
// "610 coreless motor fan" (см. 610_coreless_motor_fan.md).
// Docs: https://docs.wokwi.com/chips-api/getting-started
//
// Пины модуля (согласно шелкографии реального модуля):
//   V (VCC)    — питание 3..5 В
//   G (GND)    — земля
//   S (Signal) — ШИМ-сигнал на встроенный MOSFET-драйвер (0..100% duty)
//
// Чип не реализует никакого протокола (I2C/SPI/UART) — это чисто силовая
// нагрузка. Он измеряет скважность (duty cycle) ШИМ-сигнала на пине S,
// усредняя время HIGH/LOW за каждое окно кадра анимации (а не по одному
// отдельному импульсу — это было бы слишком шумно), сглаживает результат
// экспоненциальным фильтром (инерция ротора) и рисует вращающийся
// пропеллер на собственном мини-дисплее чипа (Framebuffer API) — так
// достигается "визуальное вращение" мотора прямо в Wokwi, без какой-либо
// доработки на стороне прошивки.
//
// Прошивка при этом ничего не обязана знать про этот чип: она просто крутит
// обычный аппаратный ШИМ (LEDC) на пине S, как и с настоящим модулем.
//
// Максимальные обороты регулируются вручную:
// кликните на сам чип в Wokwi, откроется панель со слайдером
// "Max RPM", и её можно менять мышкой прямо во время
// симуляции — точно так же, как слайдер "Temperature (°C)" у BMP280.
// Значение слайдера — это control "maxRpm" из
// 610_coreless_motor_fan.chip.json, привязанный к одноимённому атрибуту
// через Attributes API.

#include "wokwi-api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

// Обороты холостого хода реального модуля (см. документацию) — верхняя
// граница диапазона по умолчанию, пока слайдер не подвинут.
// Настраивается через атрибут/слайдер maxRpm в diagram.json /
// 610_coreless_motor_fan.chip.json.
#define DEFAULT_MAX_RPM 45000.0f

// Постоянная времени сглаживания (инерция вращения ротора), мс.
// Настраивается через атрибут smoothingMs в diagram.json.
#define DEFAULT_SMOOTHING_MS 250.0f

// Частота перерисовки анимации пропеллера.
#define ANIM_INTERVAL_US 16667 // 60 кадров/с

// Коэффициент визуального замедления вращения (только для анимации на
// мини-дисплее чипа). 1.0 = "честная" скорость по реальным оборотам,
// меньше 1.0 — визуально медленнее. Не влияет на измеренную скважность
// ШИМ и не требует изменений в прошивке.
#define VISUAL_SPIN_SCALE 0.005f

typedef struct {
  pin_t pin_v;
  pin_t pin_g;
  pin_t pin_s;

  // --- измерение ШИМ методом накопления времени HIGH/LOW за окно кадра ---
  // Скважность одного отдельного импульса (200 мкс на 5 кГц) слишком
  // "шумная" сама по себе для плавной анимации — вместо этого на каждом
  // кадре анимации (каждые ~16.7 мс = ~83 периода ШИМ) мы делим суммарное
  // время HIGH на общее время окна. Это устойчиво даже если симулятор
  // ненадолго "задержит" доставку одного-двух фронтов: пропущенное время
  // просто продолжает относиться к последнему известному уровню пина, а
  // не даёт случайный "подброс монетки" 0%/100%, как было бы при точечном
  // опросе pin_read().
  uint32_t level;          // последний известный цифровой уровень пина S
  uint64_t t_level_start;  // когда начался текущий уровень (для накопления)
  uint64_t acc_high_ns;    // накоплено времени HIGH с начала текущего окна
  uint64_t acc_low_ns;     // накоплено времени LOW с начала текущего окна
  float    duty_smooth;    // сглаженная (по инерции) скважность, 0..1 — то,
                            // что реально используется для анимации

  // --- анимация ---
  buffer_t fb;
  uint32_t fb_w;
  uint32_t fb_h;
  uint8_t *img;            // локальный RGBA-буфер кадра (w*h*4 байт)
  float    angle;          // текущий угол поворота пропеллера, радианы
  uint64_t t_frame_last;   // время последнего кадра анимации

  uint32_t attr_max_rpm;   // id атрибута maxRpm (слайдер) — опрашивается
                            // на каждом кадре, чтобы движение мышкой во
                            // время симуляции применялось "на лету"
  float    max_rpm;        // текущее значение maxRpm
  float    smoothing_s;    // атрибут smoothingMs, переведённый в секунды
  timer_t  anim_timer;
} chip_state_t;

static void on_pin_change(void *user_data, pin_t pin, uint32_t value);
static void on_anim_timer(void *user_data);
static void render_frame(chip_state_t *chip, float duty);
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

  chip->pin_v = pin_init("V", INPUT);
  chip->pin_g = pin_init("G", INPUT);
  chip->pin_s = pin_init("S", INPUT);

  // "maxRpm" — это control-слайдер (см. 610_coreless_motor_fan.chip.json),
  // его же значение читаем через Attributes API. Сохраняем id атрибута,
  // чтобы периодически перечитывать текущее положение слайдера в
  // on_anim_timer() — не только один раз при старте.
  chip->attr_max_rpm = attr_init_float("maxRpm", DEFAULT_MAX_RPM);
  chip->max_rpm = attr_read_float(chip->attr_max_rpm);
  if (chip->max_rpm <= 0.0f) {
    chip->max_rpm = DEFAULT_MAX_RPM;
  }

  float smoothing_ms = attr_read_float(attr_init_float("smoothingMs", DEFAULT_SMOOTHING_MS));
  if (smoothing_ms < 0.0f) smoothing_ms = 0.0f;
  chip->smoothing_s = smoothing_ms / 1000.0f;

  // Начальное состояние — по текущему статическому уровню пина S.
  chip->level = pin_read(chip->pin_s);
  chip->duty_smooth = (chip->level == HIGH) ? 1.0f : 0.0f;
  chip->acc_high_ns = 0;
  chip->acc_low_ns = 0;

  const pin_watch_config_t watch_config = {
    .edge = BOTH,
    .pin_change = on_pin_change,
    .user_data = chip,
  };
  pin_watch(chip->pin_s, &watch_config);

  chip->fb = framebuffer_init(&chip->fb_w, &chip->fb_h);
  chip->img = malloc((size_t)chip->fb_w * chip->fb_h * 4);

  uint64_t now = get_sim_nanos();
  chip->t_level_start = now;
  chip->t_frame_last = now;
  chip->angle = 0.0f;

  const timer_config_t anim_timer_config = {
    .callback = on_anim_timer,
    .user_data = chip,
  };
  chip->anim_timer = timer_init(&anim_timer_config);
  timer_start(chip->anim_timer, ANIM_INTERVAL_US, true);

  render_frame(chip, chip->duty_smooth);

  printf("610 coreless motor fan (симуляция): maxRpm=%.0f, сглаживание=%.0f мс, пин сигнала S\n",
         chip->max_rpm, smoothing_ms);
}

// Вызывается при каждом изменении уровня на пине S. Мы не пытаемся
// посчитать скважность по одному импульсу — вместо этого просто копим,
// сколько времени пин провёл HIGH и сколько LOW, с начала текущего окна
// (окно — это интервал между кадрами анимации, см. on_anim_timer).
static void on_pin_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *chip = (chip_state_t *)user_data;
  uint64_t now = get_sim_nanos();
  uint64_t dt = now - chip->t_level_start;

  if (chip->level == HIGH) {
    chip->acc_high_ns += dt;
  } else {
    chip->acc_low_ns += dt;
  }

  chip->level = value;
  chip->t_level_start = now;
}

static void on_anim_timer(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  uint64_t now = get_sim_nanos();

  // Закрываем текущее окно измерения: доучитываем время от последнего
  // известного фронта до "сейчас" в счёт того уровня, на котором пин
  // сейчас находится (даже если за это время не было ни одного фронта —
  // например, при duty 0% или 100% вообще нет переключений).
  uint64_t tail = now - chip->t_level_start;
  uint64_t high_ns = chip->acc_high_ns + (chip->level == HIGH ? tail : 0);
  uint64_t low_ns  = chip->acc_low_ns  + (chip->level == LOW  ? tail : 0);
  uint64_t total_ns = high_ns + low_ns;

  float duty_raw = (total_ns > 0) ? (float)high_ns / (float)total_ns : chip->duty_smooth;
  if (duty_raw < 0.0f) duty_raw = 0.0f;
  if (duty_raw > 1.0f) duty_raw = 1.0f;

  // Открываем новое окно с этого момента.
  chip->acc_high_ns = 0;
  chip->acc_low_ns = 0;
  chip->t_level_start = now;

  float dt_s = (float)(now - chip->t_frame_last) / 1e9f;
  chip->t_frame_last = now;
  if (dt_s < 0.0f || dt_s > 0.5f) {
    dt_s = 0.0f; // защита от аномальных скачков времени симуляции
  }

  // Экспоненциальное сглаживание — придаёт ротору "инерцию", чтобы даже
  // реальное изменение скорости выглядело как плавный разгон/торможение,
  // а не мгновенный скачок.
  if (chip->smoothing_s > 0.0f && dt_s > 0.0f) {
    float alpha = dt_s / (chip->smoothing_s + dt_s);
    chip->duty_smooth += alpha * (duty_raw - chip->duty_smooth);
  } else {
    chip->duty_smooth = duty_raw;
  }

  // Перечитываем положение слайдера "Max RPM" — пользователь может двигать
  // его прямо во время симуляции, как и слайдер температуры у BMP280.
  // Если слайдер ещё не трогали, здесь просто вернётся текущее значение.
  float max_rpm_now = attr_read_float(chip->attr_max_rpm);
  if (max_rpm_now > 0.0f) {
    chip->max_rpm = max_rpm_now;
  }

  float rpm = chip->duty_smooth * chip->max_rpm;
  float angular_speed = rpm * 2.0f * PI_F / 60.0f * VISUAL_SPIN_SCALE; // рад/с
  chip->angle = angle_wrap(chip->angle + angular_speed * dt_s);

  render_frame(chip, chip->duty_smooth);
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

static void render_frame(chip_state_t *chip, float duty) {
  if (!chip->img) return;

  const float w = (float)chip->fb_w;
  const float h = (float)chip->fb_h;
  const float cx = w / 2.0f;
  const float cy = h / 2.0f;
  const float r_outer = (w < h ? w : h) / 2.0f - 2.0f; // внешний край корпуса
  const float r_frame_inner = r_outer * 0.88f;         // край обода решётки
  const float hub_r = r_outer * 0.16f;                 // ступица
  const float blade_r = r_outer * 0.80f;               // длина лопасти
  const float blade_half_width = 0.34f;                // рад, у ступицы
  const float blade_curve = 0.55f;                      // рад, изгиб к концу
  const int blades = 3;

  // Цвет лопастей: серый (мотор выключен) -> тёплый оранжевый (макс. скорость)
  uint8_t blade_r8 = (uint8_t)lerp(130.0f, 255.0f, duty);
  uint8_t blade_g8 = (uint8_t)lerp(130.0f, 150.0f, duty);
  uint8_t blade_b8 = (uint8_t)lerp(130.0f, 40.0f, duty);

  for (int y = 0; y < (int)chip->fb_h; y++) {
    for (int x = 0; x < (int)chip->fb_w; x++) {
      float fx = (float)x - cx;
      float fy = (float)y - cy;
      float r = sqrtf(fx * fx + fy * fy);
      float theta = atan2f(fy, fx);

      uint8_t r8 = 0, g8 = 0, b8 = 0, a8 = 0;

      if (r <= r_outer) {
        if (r > r_frame_inner) {
          // пластиковый корпус/обод
          r8 = 205; g8 = 205; b8 = 210; a8 = 255;
        } else {
          // тёмное "нутро" вентилятора за решёткой
          r8 = 25; g8 = 25; b8 = 28; a8 = 255;
        }

        // лопасти
        if (r >= hub_r && r <= blade_r) {
          float frac = (r - hub_r) / (blade_r - hub_r);
          float half_w = blade_half_width * (1.0f - 0.35f * frac);
          for (int k = 0; k < blades; k++) {
            float base_angle = (float)k * (2.0f * PI_F / (float)blades);
            float center_angle = angle_wrap(chip->angle + base_angle + blade_curve * frac);
            float d = angle_diff(theta, center_angle);
            if (fabsf(d) < half_w) {
              r8 = blade_r8; g8 = blade_g8; b8 = blade_b8; a8 = 255;
              break;
            }
          }
        }

        // ступица мотора поверх лопастей
        if (r <= hub_r) {
          float shade = 1.0f - (r / hub_r) * 0.4f;
          r8 = (uint8_t)(45.0f * shade);
          g8 = (uint8_t)(45.0f * shade);
          b8 = (uint8_t)(50.0f * shade);
          a8 = 255;
        }
      }

      set_px(chip, x, y, r8, g8, b8, a8);
    }
  }

  buffer_write(chip->fb, 0, chip->img, chip->fb_w * chip->fb_h * 4);
}