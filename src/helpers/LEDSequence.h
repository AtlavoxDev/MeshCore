#pragma once

#include <Arduino.h>
#include <stdint.h>

// Canonical boot / power-off LED cues. Opt-in shared helper.
// Boot: both LEDs on for the duration of setup(); short closing flash
// on the secondary LED when onBootComplete() is called.
//
// Brightness: each LED has an optional brightness_pct (0-100, default 100).
// At 100 the helper uses digitalWrite for clean on/off; below 100 it uses
// analogWrite (PWM). Important: on the Adafruit nRF52 core, once a pin is
// driven via analogWrite it's PWM-bound; subsequent digitalWrite on that
// pin is silently ignored. So if you reduce a pin's brightness here, any
// OTHER helper that touches the same pin (e.g., HoldButton's feedback LED,
// radio TX LED toggling) must also write via analogWrite to keep the pin
// responsive. Easiest rule: match brightness_pct across all helpers using
// the same pin.
class LEDSequence {
public:
  struct Config {
    int8_t  primary_pin             = -1;    // -1 disables LED output
    int8_t  secondary_pin           = -1;    // -1 for single-LED boards; primary is reused for closing flash
    int8_t  buzzer_pin              = -1;    // -1 to skip buzzer cues
    uint8_t active_level            = HIGH;  // LOW for ~12 nRF52 boards w/ inverted LEDs
    uint8_t primary_brightness_pct  = 100;   // 0-100; <100 forces analogWrite (PWM)
    uint8_t secondary_brightness_pct = 100;  // 0-100; <100 forces analogWrite (PWM)
  };

  static void begin(const Config& cfg);

  // Turn both LEDs on. Returns immediately. LEDs stay on through the rest
  // of setup() until onBootComplete() turns them off and runs the flash.
  static void playBoot();

  // LEDs off, brief gap, secondary LED flash, off. Synchronous (~300ms).
  static void onBootComplete();

  // Turn LEDs off. Used before code that needs exclusive control of the
  // LED pins (e.g., powerOff()'s shutdown cue).
  static void cancel();

  // Power-off cue (synchronous, ~1.05s): primary solid 1s → both flash 50ms → off.
  static void playPowerOff();
};
