#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * PowerButton — hold-to-power-off detection with progressive LED feedback.
 *
 * Reads a configured GPIO every poll() call, tracks hold duration, drives
 * an optional LED for mid-hold "are you sure?" feedback, and returns true
 * once the button has been held continuously past the threshold. Boards
 * call this from their pollButton() override; the return value is the
 * signal to invoke board.powerOff().
 *
 * Assumes active-low buttons with internal pull-up (the convention used
 * by every existing MeshCore variant). Boards must call pinMode(pin,
 * INPUT_PULLUP) themselves — PowerButton::begin() does not configure the
 * button pin so it doesn't fight with board-level pin setup.
 *
 * Visual cadence (threshold split into 2 measures × 4 beats = 8 beats):
 *   Measure 1, beat 1: single brief blink ("I see you" cue)
 *   Measure 1, beats 2-4: dark
 *   Measure 2, every beat: flash (4 escalating flashes)
 *   Measure 3, beat 1: caller invokes powerOff()
 *
 * Pattern proportions scale cleanly to any threshold (2000 ms => 250 ms
 * beats, 1500 ms => ~187 ms beats, etc).
 *
 * If feedback_pin is -1, polling still works (returns true at threshold)
 * but no visual feedback is drawn — useful for 0-LED boards that want
 * hold-to-power-off without any UI.
 */
class PowerButton {
public:
  struct Config {
    int8_t   pin           = -1;     ///< button GPIO. -1 disables polling entirely.
    uint32_t threshold_ms  = 2000;   ///< hold duration to commit.
    int8_t   feedback_pin  = -1;     ///< LED for mid-hold feedback. -1 = no visual.
    uint8_t  active_level  = HIGH;   ///< feedback LED active level (HIGH on most boards).
  };

  /// Configure pin mapping. Safe to call with pin = -1 on boards without
  /// a power button — poll() then becomes a no-op that always returns false.
  static void begin(const Config& cfg);

  /// Call once per loop iteration. Returns true at the moment the button
  /// has been held for >= threshold_ms — the caller should then invoke
  /// board.powerOff() (which is expected to not return).
  ///
  /// Released before threshold: feedback LED clears, internal hold tracker
  /// resets, returns false. Subsequent presses start over from beat 1.1.
  static bool poll();
};
