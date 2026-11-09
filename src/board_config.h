#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/*
 * Распиновка платы в зависимости от чипа.
 * Целевой чип определяется автоматически (CONFIG_IDF_TARGET_*), его задаёт
 * выбранное окружение PlatformIO:
 *   heltec_wifi_lora_32_V3  -> ESP32-S3 (по умолчанию)
 *   esp32                   -> классический ESP32
 *
 * Любой пин можно переопределить без правки кода, например в platformio.ini:
 *   build_flags = -DI2C_MASTER_SDA_IO=GPIO_NUM_21 -DDRV8837_IN1_PIN=GPIO_NUM_25
 */

#include "sdkconfig.h"
#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32S3
/* ---- ESP32-S3: Heltec WiFi LoRa 32 V3 / V4 ---- */
#define BOARD_TARGET_NAME "ESP32-S3"

#ifndef I2C_MASTER_SDA_IO
#define I2C_MASTER_SDA_IO GPIO_NUM_17   /* SDA (OLED + BMP280) */
#endif
#ifndef I2C_MASTER_SCL_IO
#define I2C_MASTER_SCL_IO GPIO_NUM_18   /* SCL (OLED + BMP280) */
#endif
#ifndef OLED_RST
#define OLED_RST          GPIO_NUM_21   /* reset OLED */
#endif
#ifndef VEXT_PIN
#define VEXT_PIN          GPIO_NUM_36   /* питание периферии, LOW = включено */
#endif
#ifndef DRV8837_IN1_PIN
#define DRV8837_IN1_PIN    GPIO_NUM_4    /* DRV8837 IN1 (PWM) */
#endif
#ifndef DRV8837_IN2_PIN
#define DRV8837_IN2_PIN    GPIO_NUM_5    /* DRV8837 IN2 (держим LOW) */
#endif
#ifndef DRV8837_NSLEEP_PIN
#define DRV8837_NSLEEP_PIN GPIO_NUM_6    /* DRV8837 nSLEEP, HIGH = активен */
#endif

#elif CONFIG_IDF_TARGET_ESP32
/* ---- Классический ESP32: Heltec WiFi LoRa 32 V2 ----
 * На ESP32 GPIO34..39 работают только как входы, поэтому пины S3
 * (в частности GPIO36 для Vext) здесь использовать нельзя. */
#define BOARD_TARGET_NAME "ESP32"

#ifndef I2C_MASTER_SDA_IO
#define I2C_MASTER_SDA_IO GPIO_NUM_4
#endif
#ifndef I2C_MASTER_SCL_IO
#define I2C_MASTER_SCL_IO GPIO_NUM_15
#endif
#ifndef OLED_RST
#define OLED_RST          GPIO_NUM_16
#endif
#ifndef VEXT_PIN
#define VEXT_PIN          GPIO_NUM_21   /* LOW = включено */
#endif
#ifndef DRV8837_IN1_PIN
#define DRV8837_IN1_PIN    GPIO_NUM_13   /* свободный пин, не strapping и не LoRa */
#endif
#ifndef DRV8837_IN2_PIN
#define DRV8837_IN2_PIN    GPIO_NUM_17
#endif
#ifndef DRV8837_NSLEEP_PIN
#define DRV8837_NSLEEP_PIN GPIO_NUM_23
#endif

#else
#error "Неподдерживаемый чип: проект рассчитан на ESP32 и ESP32-S3"
#endif

#endif // BOARD_CONFIG_H
