#pragma once

#include <Arduino.h>
#include <stdint.h>

// Canonical boot / power-off LED choreography. Opt-in shared helper.
// NRF52/ESP32: animation runs async via hardware timer; RP2040/STM32:
// degrades to static LEDs-on cue + final flash. Gotcha: never analogWrite()
// on a pin handed to this helper (PWM-poisons it on the Adafruit nRF52 core).
class LEDSequence {
public:
  struct Config {
    int8_t  primary_pin   = -1;    // -1 disables LED output
    int8_t  secondary_pin = -1;    // -1 for single-LED boards
    int8_t  buzzer_pin    = -1;    // -1 to skip buzzer cues
    uint8_t active_level  = HIGH;  // LOW for ~12 nRF52 boards w/ inverted LEDs
  };

  static void begin(const Config& cfg);

  // Boot sequence: BRIGHT 1s → DARK 1s → FLICKER (until onBootComplete or
  // 15s safety) → DARK 1s → FLASH 100ms → off. Returns immediately.
  static void playBoot();

  // Signal the FLICKER phase to exit; remaining phases play in background.
  static void onBootComplete();

  // Hard-stop the boot sequence — use before code that needs exclusive
  // control of the LED pins (e.g., powerOff()'s shutdown cue).
  static void cancel();

  // Power-off cue (synchronous, ~1.05s): primary solid 1s → both flash 50ms → off.
  static void playPowerOff();
};
