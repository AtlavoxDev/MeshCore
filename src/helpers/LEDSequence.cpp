#include "LEDSequence.h"

#include <Arduino.h>

static constexpr uint32_t BOOT_GAP_MS            = 200;   // gap between BRIGHT end and FLASH
static constexpr uint32_t BOOT_FLASH_MS          = 100;   // closing flash duration
static constexpr uint32_t POWEROFF_SOLID_MS      = 1000;
static constexpr uint32_t POWEROFF_DUAL_FLASH_MS = 50;

static LEDSequence::Config s_cfg     = {};
static bool                s_enabled = false;
static bool                s_done    = false;  // prevents double onBootComplete()

// Write a pin to on/off, choosing digitalWrite vs analogWrite based on
// brightness_pct. 100 => digitalWrite. <100 => analogWrite (PWM).
// Once a pin is written via analogWrite on nRF52, it stays PWM-bound —
// don't mix brightness levels across helpers writing the same pin.
static inline void writePin(int8_t pin, bool on, uint8_t brightness_pct) {
  if (pin < 0) return;
  if (brightness_pct >= 100) {
    digitalWrite(pin, on ? s_cfg.active_level : !s_cfg.active_level);
  } else {
    uint8_t pwm = on ? (uint8_t)((uint16_t)brightness_pct * 255U / 100U) : 0;
    if (s_cfg.active_level == LOW) pwm = 255 - pwm;
    analogWrite(pin, pwm);
  }
}

static inline void setLEDs(bool primary_on, bool secondary_on) {
  writePin(s_cfg.primary_pin,   primary_on,  s_cfg.primary_brightness_pct);
  writePin(s_cfg.secondary_pin, secondary_on, s_cfg.secondary_brightness_pct);
}

static inline int8_t flashPin() {
  return (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin : s_cfg.primary_pin;
}

static inline uint8_t flashBrightnessPct() {
  return (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_brightness_pct
                                     : s_cfg.primary_brightness_pct;
}

void LEDSequence::begin(const LEDSequence::Config& cfg) {
  s_cfg     = cfg;
  s_enabled = (cfg.primary_pin >= 0) || (cfg.buzzer_pin >= 0);
  s_done    = false;

  if (cfg.primary_pin >= 0) {
    pinMode(cfg.primary_pin, OUTPUT);
    writePin(cfg.primary_pin, false, cfg.primary_brightness_pct);
  }
  if (cfg.secondary_pin >= 0) {
    pinMode(cfg.secondary_pin, OUTPUT);
    writePin(cfg.secondary_pin, false, cfg.secondary_brightness_pct);
  }
  // TODO: buzzer pin init when buzzer integration lands
}

void LEDSequence::playBoot() {
  if (!s_enabled || s_done) return;
  setLEDs(true, true);  // LEDs stay on for the duration of setup()
}

void LEDSequence::onBootComplete() {
  if (!s_enabled || s_done) return;
  s_done = true;

  setLEDs(false, false);
  delay(BOOT_GAP_MS);
  writePin(flashPin(), true, flashBrightnessPct());
  delay(BOOT_FLASH_MS);
  writePin(flashPin(), false, flashBrightnessPct());
}

void LEDSequence::cancel() {
  if (!s_enabled) return;
  setLEDs(false, false);
  s_done = true;  // suppresses any subsequent onBootComplete()
}

void LEDSequence::playPowerOff() {
  if (!s_enabled) return;
  cancel();  // make sure boot LEDs are off before we take over the pins

  // SOLID: primary on, secondary off, 1s
  writePin(s_cfg.primary_pin,   true,  s_cfg.primary_brightness_pct);
  writePin(s_cfg.secondary_pin, false, s_cfg.secondary_brightness_pct);
  delay(POWEROFF_SOLID_MS);

  // DUAL_FLASH: both on, 50ms
  writePin(s_cfg.primary_pin,   true, s_cfg.primary_brightness_pct);
  writePin(s_cfg.secondary_pin, true, s_cfg.secondary_brightness_pct);
  delay(POWEROFF_DUAL_FLASH_MS);

  setLEDs(false, false);
  // TODO: buzzer descending shutdown tone
}
