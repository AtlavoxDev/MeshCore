#pragma once

#include <Arduino.h>
#include <stdint.h>

// Alternating two-LED pattern shown while the device is BLE-discoverable
// (advertising, no client connected). Stops when a client connects.
//
// Opt-in: boards configure pins in begin(); the example firmware calls
// poll(is_pairing) each loop iteration to drive the pattern.
//
// Brightness handling matches LEDSequence: brightness_pct >= 100 uses
// digitalWrite, below 100 uses analogWrite (PWM). If a pin is shared with
// LEDSequence (the same primary/secondary LEDs), keep brightness_pct
// consistent across both helpers — see LEDSequence.h for the nRF52
// PWM-poisoning rule.
class BLEPairingIndicator {
public:
  struct Config {
    int8_t  primary_pin              = -1;   // first LED (e.g., red)
    int8_t  secondary_pin            = -1;   // second LED (e.g., blue)
    uint8_t active_level             = HIGH;
    uint8_t primary_brightness_pct   = 100;
    uint8_t secondary_brightness_pct = 100;
    uint16_t toggle_ms               = 250;  // half-period (red 250ms, then blue 250ms)
  };

  static void begin(const Config& cfg);

  // Call once per loop iteration. When is_pairing goes false→true the
  // alternation starts; true→false turns both LEDs off cleanly. While true,
  // toggles between primary and secondary at toggle_ms intervals.
  static void poll(bool is_pairing);
};
