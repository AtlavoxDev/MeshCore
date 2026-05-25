#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * HoldButton — generic hold-to-trigger detection with progressive LED feedback.
 *
 * Reads a configured GPIO every poll() call, tracks hold duration, drives
 * an optional LED for mid-hold "are you sure?" feedback, and returns true
 * once the button has been held continuously past the threshold. The caller
 * decides what to do with the trigger:
 *
 *   if (HoldButton::poll()) {
 *     powerOff();           // hold-to-power-off (most common use)
 *   }
 *
 *   // or just as easily:
 *   if (HoldButton::poll()) {
 *     enterPairingMode();   // hold-to-pair
 *   }
 *
 * The class is intentionally purpose-agnostic. M6 happens to use it for
 * power-off, but any board feature that wants "hold the button for N
 * milliseconds" with consistent visual feedback can use it.
 *
 * Assumes active-low buttons with internal pull-up (the convention used by
 * every existing MeshCore variant). Boards must call pinMode(pin,
 * INPUT_PULLUP) themselves — begin() does not configure the button pin so
 * it doesn't fight with board-level pin setup.
 *
 * Visual cadence (threshold split into 2 measures × 4 beats = 8 beats):
 *   Measure 1, beat 1: single brief blink ("I see you" cue)
 *   Measure 1, beats 2-4: dark
 *   Measure 2, every beat: flash (4 escalating flashes)
 *   Measure 3, beat 1: poll() returns true; caller acts on trigger
 *
 * Pattern proportions scale cleanly to any threshold (2000 ms => 250 ms
 * beats, 1500 ms => ~187 ms beats, etc).
 *
 * Relationship to MomentaryButton:
 *   src/helpers/ui/MomentaryButton handles click/double-click/triple-click/
 *   long-press *edge events*. HoldButton handles *progressive mid-hold
 *   state* — what MomentaryButton would need a heldFor() accessor to
 *   support. Use MomentaryButton for click-count UX, HoldButton for
 *   hold-with-feedback UX. They can coexist on the same physical button
 *   if a board wants both (with care around shared digitalRead overhead).
 *
 * Singleton: one HoldButton per board. If you ever need multiple hold-
 * detect buttons on the same device, this helper would need to be
 * re-architected around instances. No current board needs that.
 */
class HoldButton {
public:
  struct Config {
    int8_t   pin           = -1;     ///< button GPIO. -1 disables polling entirely.
    uint32_t threshold_ms  = 2000;   ///< hold duration to trigger.
    int8_t   feedback_pin  = -1;     ///< LED for mid-hold feedback. -1 = no visual.
    uint8_t  active_level  = HIGH;   ///< feedback LED active level (HIGH on most boards).
  };

  /// Configure pin mapping. Safe to call with pin = -1 on boards without
  /// a hold-detect button — poll() then becomes a no-op that always returns false.
  static void begin(const Config& cfg);

  /// Call once per loop iteration. Returns true at the moment the button
  /// has been held for >= threshold_ms. Released before threshold: feedback
  /// LED clears, internal hold tracker resets, returns false. Subsequent
  /// presses start over from beat 1.1.
  static bool poll();
};
