#pragma once

#include <Arduino.h>
#include <stdint.h>

// Canonical boot / power-off LED choreography. Opt-in shared helper.
// Boot: both LEDs on for 1s, then off; secondary LED briefly flashes when
// onBootComplete() is called. NRF52/ESP32 run the 1s BRIGHT phase via a
// hardware timer so it doesn't block setup(); other platforms degrade to
// a synchronous cue at boot end.
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

  // Both LEDs on for 1s, then off. Returns immediately (async on NRF52/ESP32).
  static void playBoot();

  // Triggers the closing flash (secondary LED on for ~100ms). If BRIGHT is
  // still running, the flash is queued and runs immediately after BRIGHT ends.
  static void onBootComplete();

  // Hard-stop the boot sequence — use before code that needs exclusive
  // control of the LED pins (e.g., powerOff()'s shutdown cue).
  static void cancel();

  // Power-off cue (synchronous, ~1.05s): primary solid 1s → both flash 50ms → off.
  static void playPowerOff();
};
