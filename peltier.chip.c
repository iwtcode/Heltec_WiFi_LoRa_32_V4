// Wokwi Custom Chip - a visual model of a two-wire Peltier element.
//
// Pins:
//   V+ - positive supply terminal
//   V- - negative supply terminal
//
// Wokwi does not simulate thermal transfer or load current. This model only
// visualizes the element and its polarity: blue marks the cold side and red
// marks the hot side. Reversing the polarity swaps the two sides.

#include "wokwi-api.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  pin_t pin_positive;
  pin_t pin_negative;
  buffer_t framebuffer;
  uint32_t width;
  uint32_t height;
  uint8_t *pixels;
} chip_state_t;

static void render(chip_state_t *chip);

static void set_pixel(chip_state_t *chip, int x, int y,
                      uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
  if (x < 0 || y < 0 || (uint32_t)x >= chip->width || (uint32_t)y >= chip->height) {
    return;
  }

  const size_t offset = ((size_t)y * chip->width + (size_t)x) * 4;
  chip->pixels[offset + 0] = red;
  chip->pixels[offset + 1] = green;
  chip->pixels[offset + 2] = blue;
  chip->pixels[offset + 3] = alpha;
}

static void fill_rect(chip_state_t *chip, int x0, int y0, int x1, int y1,
                      uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      set_pixel(chip, x, y, red, green, blue, alpha);
    }
  }
}

static void draw_letter(chip_state_t *chip, int x0, int y0, const uint8_t rows[7]) {
  const int scale = 3;
  for (int row = 0; row < 7; row++) {
    for (int col = 0; col < 5; col++) {
      if (rows[row] & (1u << (4 - col))) {
        fill_rect(chip,
                  x0 + col * scale, y0 + row * scale,
                  x0 + (col + 1) * scale, y0 + (row + 1) * scale,
                  35, 42, 52, 255);
      }
    }
  }
}

static void on_power_change(void *user_data, pin_t pin, uint32_t value) {
  (void)pin;
  (void)value;
  render((chip_state_t *)user_data);
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  memset(chip, 0, sizeof(chip_state_t));

  chip->pin_positive = pin_init("V+", INPUT);
  chip->pin_negative = pin_init("V-", INPUT);
  chip->framebuffer = framebuffer_init(&chip->width, &chip->height);
  chip->pixels = malloc((size_t)chip->width * chip->height * 4);

  const pin_watch_config_t watch_config = {
    .user_data = chip,
    .edge = BOTH,
    .pin_change = on_power_change,
  };
  pin_watch(chip->pin_positive, &watch_config);
  pin_watch(chip->pin_negative, &watch_config);

  render(chip);
}

static void render(chip_state_t *chip) {
  static const uint8_t letter_t[7] = {
    0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  };
  static const uint8_t letter_e[7] = {
    0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f,
  };
  static const uint8_t letter_c[7] = {
    0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f,
  };

  memset(chip->pixels, 0, (size_t)chip->width * chip->height * 4);

  const bool forward = pin_read(chip->pin_positive) == HIGH &&
                       pin_read(chip->pin_negative) == LOW;
  const bool reverse = pin_read(chip->pin_positive) == LOW &&
                       pin_read(chip->pin_negative) == HIGH;
  const bool active = forward || reverse;

  // Outer ceramic plate and dark edge.
  fill_rect(chip, 5, 5, 95, 95, 55, 62, 72, 255);
  fill_rect(chip, 8, 8, 92, 92, 235, 238, 242, 255);

  // Polarity/temperature indicator. When unpowered both bands stay neutral.
  if (!active) {
    fill_rect(chip, 12, 12, 88, 27, 150, 157, 166, 255);
    fill_rect(chip, 12, 73, 88, 88, 150, 157, 166, 255);
  } else if (forward) {
    fill_rect(chip, 12, 12, 88, 27, 48, 145, 230, 255);  // cold
    fill_rect(chip, 12, 73, 88, 88, 230, 72, 62, 255);   // hot
  } else {
    fill_rect(chip, 12, 12, 88, 27, 230, 72, 62, 255);   // hot
    fill_rect(chip, 12, 73, 88, 88, 48, 145, 230, 255);  // cold
  }

  draw_letter(chip, 22, 39, letter_t);
  draw_letter(chip, 43, 39, letter_e);
  draw_letter(chip, 64, 39, letter_c);

  buffer_write(chip->framebuffer, 0, chip->pixels,
               (uint32_t)((size_t)chip->width * chip->height * 4));
}

