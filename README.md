# 📟 Heltec WiFi LoRa 32 V4 — BMP280 + OLED + вентилятор

<p align="center">
  <img src="images/wokwi_diagram.png" alt="Heltec WiFi LoRa 32 V4" width="40%" />
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-ESP32--S3-blue" />
  <img alt="Framework" src="https://img.shields.io/badge/framework-ESP--IDF-red" />
  <img alt="Build" src="https://img.shields.io/badge/build-PlatformIO-orange" />
  <img alt="Sim" src="https://img.shields.io/badge/simulation-Wokwi-9cf" />
</p>

Прошивка на **ESP-IDF** для платы **Heltec WiFi LoRa 32 V4**: опрашивает датчик температуры **BMP280** по I2C, выводит показания на **OLED-дисплей SSD1306** и управляет скоростью вентилятора **motor610** через ШИМ в зависимости от температуры. Проект собирается через **VS Code + PlatformIO** и полностью симулируется в **Wokwi**.

---

## 📋 Содержание

1. [Возможности](#-возможности)
2. [Схема подключения](#-схема-подключения)
3. [Настройка PlatformIO](#1-настройка-platformio)
4. [Настройка Wokwi](#2-настройка-wokwi)
5. [Сборка и загрузка](#3-сборка-и-загрузка)
6. [Возникают ошибки?](#-возникают-ошибки)

---

## ✨ Возможности

- Чтение температуры с **BMP280** по I2C
- Вывод температуры, скорости вентилятора и счётчика измерений на **OLED SSD1306**
- Автоматическое управление вентилятором **motor610** через аппаратный ШИМ
- Опрос датчика каждую секунду с логированием в UART
- Полная симуляция в **Wokwi**
- Настраиваемый диапазон, период изменения температуры и параметры вентилятора

---

## 🔌 Схема подключения

| Сигнал | Pin ESP32-S3 | OLED SSD1306 | BMP280 | motor610 |
|--------|:---:|:---:|:---:|:---:|
| SDA    | GPIO17 | SDA | SDA | — |
| SCL    | GPIO18 | SCL | SCL | — |
| 3V3    | 3V3    | VCC | VCC, CSB | V |
| GND    | GND    | GND | GND, SDO | G |
| Reset  | GPIO21 | RST | — | — |
| VEXT   | GPIO36 | (питание платы) | — | — |
| PWM (сигнал) | GPIO4 | — | — | S |

> `SDO → GND` задаёт I2C-адрес BMP280 `0x76`; при `SDO → VCC` адрес будет `0x77`

> Вентилятор **motor610** — coreless-моторчик с встроенным MOSFET-драйвером на плате модуля: питание (`V`/`G`) берётся напрямую с линии 3V3/GND, а пин `S` принимает слабый ШИМ-сигнал с GPIO4. Подробные характеристики и распиновка модуля — в [`motor610.md`](motor610.md).

---

## 1. настройка PlatformIO

Установите расширение **PlatformIO** через маркетплейс VS Code

`VS Code` → `Extensions` → `PlatformIO` → `Download`

Откройте терминал PlatformIO одним из способов

- `PlatformIO` → `Quick Access` → `Miscellaneous` → `New Terminal`
- `VS Code` → `Ctrl + Shift + P` → `PlatformIO: New Terminal`

Затем выполните команды

```bash
pio project init -b heltec_wifi_lora_32_V3 -O "framework=espidf"
pio pkg update
```

---

## 2. Настройка Wokwi

*Wokwi — расширение VS Code для симуляции платы без физического устройства*

Установите расширение **Wokwi** через маркетплейс VS Code

`VS Code` → `Extensions` → `Wokwi Simulator` → `Download`

Скомпилируйте файлы кастомных чипов

```bash
iwr https://wokwi.com/ci/install.ps1 -useb | iex
wokwi-cli chip compile bmp280.chip.c -o bmp280.chip.wasm
wokwi-cli chip compile motor610.chip.c -o motor610.chip.wasm
```

Параметры симуляции задаются в `diagram.json`.

Для чипа `chip-bmp280`:

| Параметр | Назначение | По умолчанию |
|---|---|---|
| `minTemperature` | нижняя граница диапазона, °C | 18 |
| `maxTemperature` | верхняя граница диапазона, °C | 30 |
| `updateIntervalMs` | период обновления значения, мс | 2000 |

Для чипа `chip-motor610`:

| Параметр | Назначение | По умолчанию |
|---|---|---|
| `maxRpm` | максимальная скорость вентилятора, об/мин | 45000 |
| `smoothingMs` | сглаживание изменения скорости при отклике на ШИМ, мс | 250 |

> Примечание: в Wokwi можно регулировать `maxRpm` визуально (слайдером в `diagram.json`), но в прошивке это значение задаётся константой `MOTOR610_MAX_RPM` в `motor610.h` и во время работы программно не изменяется.

---

## 3. Сборка и загрузка

Финальный шаг — собрать и загрузить проект

**`Build`** → **`Upload`**

> Смотреть логи можно через `Serial Monitor` (Ctrl+Alt+S)

✅ Готово

---

## 🛠️ Возникают ошибки?

Удалите артефакты сборки и повторите сборку заново

- `.pio`
- `.vscode`
- `managed_components`
- `sdkconfig.heltec_wifi_lora_32_V3`

```bash
pio run -t clean
```