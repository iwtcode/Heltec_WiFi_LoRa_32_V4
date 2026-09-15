# 📟 Heltec WiFi LoRa 32 V4

Пошаговая инструкция по созданию и настройке проекта для платы **Heltec WiFi LoRa 32 V4** с использованием **VS Code + PlatformIO**, включая симуляцию в **Wokwi**

---

## 📋 Содержание

1. [Установка PlatformIO](#1-установка-platformio)
2. [Создание проекта](#2-создание-проекта)
3. [Настройка Wokwi](#3-настройка-wokwi)
4. [Сборка и загрузка](#4-сборка-и-загрузка)

---

## 1. Установка PlatformIO

Установите расширение **PlatformIO** через маркетплейс VS Code

`VS Code` → `Extensions` → `PlatformIO` → `Download`

---

## 2. Создание проекта

Откройте терминал PlatformIO одним из способов

- `PlatformIO` → `Quick Access` → `Miscellaneous` → `New Terminal`
- `VS Code` → `Ctrl + Shift + P` → `PlatformIO: New Terminal`

Затем выполните команды

```bash
pio project init -b heltec_wifi_lora_32_V3 -O "framework=espidf"
pio pkg update
```

---

## 3. Настройка Wokwi

*Wokwi — расширение VS Code для симуляции платы без физического устройства*

### Компиляция (Powershell)

```bash
iwr https://wokwi.com/ci/install.ps1 -useb | iex
wokwi-cli chip compile bmp280.chip.c -o bmp280.chip.wasm
```

---

## 4. Сборка и загрузка

Финальный шаг — собрать и загрузить проект

**`Build`** → **`Upload`**

> Смотреть логи можно через `Serial Monitor` (Ctrl+Alt+S)

✅ Готово