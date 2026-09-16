// Wokwi Custom Chip — симуляционная модель датчика BMP280.
// Docs: https://docs.wokwi.com/chips-api/getting-started
//
// Реализует настоящий регистровый протокол BMP280 по I2C:
//   0xD0        — chip id
//   0xE0        — soft reset
//   0x88..0x8D  — калибровочные коэффициенты температуры (dig_T1..dig_T3)
//   0xF4        — ctrl_meas
//   0xF5        — config
//   0xFA..0xFC  — данные температуры (MSB/LSB/XLSB)
//
// Благодаря этому одна и та же прошивка ESP-IDF, работающая с реальным
// датчиком BMP280 по стандартному протоколу, без изменений работает и с
// этой симуляцией.
//
// Температура НЕ меняется случайно: она управляется вручную — кликните на
// сам чип в Wokwi, откроется панель со слайдером "Temperature (°C)", и её
// можно менять мышкой прямо во время симуляции. Значение слайдера — это
// control "temperature" из bmp280.chip.json, привязанный к одноимённому
// атрибуту через Attributes API.

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

// dig_T3 намеренно равен 0 — это делает пересчёт "целевой" температуры в
// сырые ADC-показания точным и обратимым, не нарушая совместимость: в
// прошивке используется штатная целочисленная формула компенсации Bosch,
// которая одинаково корректно работает как с этими коэффициентами, так и
// с коэффициентами реального датчика.
#define DIG_T1 27504
#define DIG_T2 26435
#define DIG_T3 0

#define REG_CHIP_ID      0xD0
#define REG_RESET        0xE0
#define REG_CTRL_MEAS    0xF4
#define REG_CONFIG       0xF5
#define REG_TEMP_MSB     0xFA
#define REG_CALIB_T1_LSB 0x88

// Значение слайдера "Temperature (°C)" по умолчанию.
#define DEFAULT_TEMPERATURE 25.0f

// Как часто чип перечитывает положение слайдера и обновляет регистры
// данных. Настраивается атрибутом updateIntervalMs в diagram.json.
#define DEFAULT_UPDATE_INTERVAL_MS 100

typedef struct {
  pin_t pin_scl;
  pin_t pin_sda;
  pin_t pin_vcc;
  pin_t pin_gnd;
  pin_t pin_csb;
  pin_t pin_sdo;

  i2c_dev_t i2c;

  uint8_t regs[256];
  uint8_t reg_ptr;
  bool reg_ptr_set;

  uint32_t attr_temperature; // control "temperature" (слайдер)
  uint32_t attr_interval;

  float current_temp;

  timer_t timer;
} chip_state_t;

static bool on_i2c_connect(void *user_data, uint32_t address, bool connect);
static uint8_t on_i2c_read(void *user_data);
static bool on_i2c_write(void *user_data, uint8_t data);
static void on_i2c_disconnect(void *user_data);
static void on_timer(void *user_data);

// Пересчитывает "целевую" температуру в сырые 20-битные ADC-показания и
// записывает их в регистры данных температуры (0xFA..0xFC), как это
// делает настоящий датчик после каждого измерения.
static void write_temperature(chip_state_t *chip, float temp_c) {
  double t5120 = (double)temp_c * 5120.0;
  double val = 16384.0 * (t5120 / (double)DIG_T2 + (double)DIG_T1 / 1024.0);
  long adc = (long)(val + 0.5);
  if (adc < 0) adc = 0;
  if (adc > 0xFFFFF) adc = 0xFFFFF;

  chip->regs[REG_TEMP_MSB + 0] = (adc >> 12) & 0xFF;
  chip->regs[REG_TEMP_MSB + 1] = (adc >> 4) & 0xFF;
  chip->regs[REG_TEMP_MSB + 2] = (adc << 4) & 0xF0;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->pin_vcc = pin_init("VCC", INPUT);
  chip->pin_gnd = pin_init("GND", INPUT);
  chip->pin_csb = pin_init("CSB", INPUT);
  chip->pin_sdo = pin_init("SDO", INPUT);
  chip->pin_scl = pin_init("SCL", INPUT);
  chip->pin_sda = pin_init("SDA", INPUT);

  for (int i = 0; i < 256; i++) chip->regs[i] = 0;
  chip->reg_ptr = 0;
  chip->reg_ptr_set = false;

  chip->regs[REG_CHIP_ID] = 0x58; // идентификатор чипа BMP280

  chip->regs[REG_CALIB_T1_LSB + 0] = DIG_T1 & 0xFF;
  chip->regs[REG_CALIB_T1_LSB + 1] = (DIG_T1 >> 8) & 0xFF;
  chip->regs[REG_CALIB_T1_LSB + 2] = DIG_T2 & 0xFF;
  chip->regs[REG_CALIB_T1_LSB + 3] = (DIG_T2 >> 8) & 0xFF;
  chip->regs[REG_CALIB_T1_LSB + 4] = DIG_T3 & 0xFF;
  chip->regs[REG_CALIB_T1_LSB + 5] = (DIG_T3 >> 8) & 0xFF;

  chip->regs[REG_CTRL_MEAS] = 0x00;
  chip->regs[REG_CONFIG] = 0x00;

  // "temperature" — это control-слайдер (см. bmp280.chip.json), его же
  // значение читаем через Attributes API. Никакого случайного блуждания
  // больше нет — чип просто всегда показывает то, что выставлено слайдером.
  chip->attr_temperature = attr_init_float("temperature", DEFAULT_TEMPERATURE);
  chip->attr_interval = attr_init("updateIntervalMs", DEFAULT_UPDATE_INTERVAL_MS);

  chip->current_temp = attr_read_float(chip->attr_temperature);
  write_temperature(chip, chip->current_temp);

  // Адрес на шине I2C: SDO->GND = 0x76 (используется по умолчанию, как и
  // в этой симуляции), SDO->VCC = 0x77 — совпадает с поведением реального
  // модуля BMP280.
  uint32_t address = pin_read(chip->pin_sdo) ? 0x77 : 0x76;

  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = address,
    .scl = chip->pin_scl,
    .sda = chip->pin_sda,
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  chip->i2c = i2c_init(&i2c_config);

  const timer_config_t timer_config = {
    .callback = on_timer,
    .user_data = chip,
  };
  chip->timer = timer_init(&timer_config);
  uint32_t interval_ms = attr_read(chip->attr_interval);
  if (interval_ms < 20) interval_ms = 20;
  timer_start(chip->timer, interval_ms * 1000, true);

  printf("BMP280 (симуляция): I2C адрес 0x%02X, температура управляется слайдером, старт %.1f C\n",
         address, chip->current_temp);
}

// Периодически перечитывает положение слайдера "temperature" и, если оно
// изменилось (пользователь подвигал мышкой), обновляет регистры данных.
static void on_timer(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  float t = attr_read_float(chip->attr_temperature);
  if (t != chip->current_temp) {
    chip->current_temp = t;
    write_temperature(chip, t);
  }
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool connect) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->reg_ptr_set = false;
  return true; // подтверждаем адрес (ACK)
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  uint8_t value = chip->regs[chip->reg_ptr];
  chip->reg_ptr = (chip->reg_ptr + 1) & 0xFF; // автоинкремент, как в реальном чипе
  return value;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  if (!chip->reg_ptr_set) {
    // Первый байт после старта транзакции — это адрес регистра
    chip->reg_ptr = data;
    chip->reg_ptr_set = true;
    return true;
  }

  if (chip->reg_ptr == REG_RESET) {
    if (data == 0xB6) { // код программного сброса
      chip->regs[REG_CTRL_MEAS] = 0x00;
      chip->regs[REG_CONFIG] = 0x00;
    }
  } else {
    chip->regs[chip->reg_ptr] = data;
  }

  chip->reg_ptr = (chip->reg_ptr + 1) & 0xFF;
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->reg_ptr_set = false;
}