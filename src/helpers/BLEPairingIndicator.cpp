#include "BLEPairingIndicator.h"

static BLEPairingIndicator::Config s_cfg              = {};
static bool                        s_active           = false;
static bool                        s_primary_on       = false;
static unsigned long               s_last_toggle_ms   = 0;

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

void BLEPairingIndicator::begin(const BLEPairingIndicator::Config& cfg) {
  s_cfg            = cfg;
  s_active         = false;
  s_primary_on     = false;
  s_last_toggle_ms = 0;
  // Pins are configured (pinMode) by LEDSequence::begin() or by the board's
  // own init code — we just write them.
}

void BLEPairingIndicator::poll(bool is_pairing) {
  if (is_pairing) {
    if (!s_active) {
      // Just entered pairing — start with primary on, secondary off
      s_active         = true;
      s_primary_on     = true;
      s_last_toggle_ms = millis();
      writePin(s_cfg.primary_pin,   true,  s_cfg.primary_brightness_pct);
      writePin(s_cfg.secondary_pin, false, s_cfg.secondary_brightness_pct);
    } else if ((unsigned long)(millis() - s_last_toggle_ms) >= s_cfg.toggle_ms) {
      // Time to alternate
      s_primary_on     = !s_primary_on;
      s_last_toggle_ms = millis();
      writePin(s_cfg.primary_pin,   s_primary_on,  s_cfg.primary_brightness_pct);
      writePin(s_cfg.secondary_pin, !s_primary_on, s_cfg.secondary_brightness_pct);
    }
  } else {
    if (s_active) {
      // Just left pairing — clear both LEDs
      s_active     = false;
      s_primary_on = false;
      writePin(s_cfg.primary_pin,   false, s_cfg.primary_brightness_pct);
      writePin(s_cfg.secondary_pin, false, s_cfg.secondary_brightness_pct);
    }
  }
}
