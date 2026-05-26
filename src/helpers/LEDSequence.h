#pragma once

#include <Arduino.h>
#include <stdint.h>

// Canonical boot / power-off LED cues. Opt-in shared helper.
// Boot: both LEDs on for the duration of setup(); short closing flash
// on the secondary LED when onBootComplete() is called.
//
// Gotcha: never analogWrite() on a pin handed to this helper (PWM-poisons
// it on the Adafruit nRF52 core).
class LEDSequence {
public:
  struct Config {
    int8_t  primary_pin   = -1;    // -1 disables LED output
    int8_t  secondary_pin = -1;    // -1 for single-LED boards; primary is reused for closing flash
    int8_t  buzzer_pin    = -1;    // -1 to skip buzzer cues
    uint8_t active_level  = HIGH;  // LOW for ~12 nRF52 boards w/ inverted LEDs
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
