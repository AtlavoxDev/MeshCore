#pragma once

#include <Arduino.h>
#include <stdint.h>

// Generic hold-to-trigger detection with beat-based mid-hold LED feedback.
// Thin wrapper over MomentaryButton (long-press event = trigger, heldFor() =
// feedback timing). Singleton — one HoldButton per board.
//
// Caller invokes whatever action on trigger:
//   if (HoldButton::poll()) powerOff();        // hold-to-power-off (M6)
//   if (HoldButton::poll()) enterPairMode();   // hold-to-pair (hypothetical)
//
// Visual cadence (threshold ÷ 8 beats):
//   M1.1: opening blink, M1.2-4: dark, M2.*: 4 escalating flashes, M3.1: trigger
class HoldButton {
public:
  struct Config {
    int8_t   pin           = -1;     // button GPIO; -1 disables poll()
    uint32_t threshold_ms  = 2000;
    int8_t   feedback_pin  = -1;     // -1 = no visual feedback
    uint8_t  active_level  = HIGH;
  };

  static void begin(const Config& cfg);

  // Call once per loop. Returns true at the moment threshold is reached.
  static bool poll();
};
