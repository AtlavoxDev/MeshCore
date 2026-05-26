#include "HoldButton.h"
#include <helpers/ui/MomentaryButton.h>

static constexpr uint32_t HOLD_BEATS_PER_THRESHOLD = 8;
static constexpr uint32_t HOLD_BEATS_PER_MEASURE   = 4;

// MomentaryButton is heap-allocated so Config's pin/threshold can be set at
// runtime rather than fixed at static-init time. One alloc per boot.
static HoldButton::Config s_cfg          = {};
static MomentaryButton*   s_btn          = nullptr;
static bool               s_feedback_on  = false;  // tracks what WE last wrote

// Only writes the pin if the desired state differs from our last write.
// Prevents stomping on other code that shares the feedback pin (e.g.,
// LEDSequence's BRIGHT phase, which uses the same pin as primary LED).
//
// Uses digitalWrite at 100% brightness, analogWrite (PWM) below. If the
// feedback pin is also driven by LEDSequence, the two helpers must use
// matching brightness_pct values to keep the pin in a consistent mode —
// see LEDSequence.h for the nRF52 PWM-poisoning rule.
static inline void writeFeedback(bool on) {
  if (on == s_feedback_on) return;
  if (s_cfg.feedback_pin >= 0) {
    if (s_cfg.feedback_brightness_pct >= 100) {
      digitalWrite(s_cfg.feedback_pin, on ? s_cfg.active_level : !s_cfg.active_level);
    } else {
      uint8_t pwm = on
        ? (uint8_t)((uint16_t)s_cfg.feedback_brightness_pct * 255U / 100U)
        : 0;
      if (s_cfg.active_level == LOW) pwm = 255 - pwm;
      analogWrite(s_cfg.feedback_pin, pwm);
    }
  }
  s_feedback_on = on;
}

void HoldButton::begin(const HoldButton::Config& cfg) {
  s_cfg = cfg;

  if (s_btn) { delete s_btn; s_btn = nullptr; }
  if (cfg.pin < 0) return;

  // active-low button, INPUT_PULLUP, no multi-click tracking
  s_btn = new MomentaryButton(cfg.pin, (int)cfg.threshold_ms, true, true, false);
  s_btn->begin();
}

bool HoldButton::poll() {
  if (!s_btn) return false;

  const int ev = s_btn->check();  // updates internal down_at that heldFor() reads

  if (ev == BUTTON_EVENT_LONG_PRESS) {
    writeFeedback(false);
    return true;  // caller invokes whatever action
  }

  const unsigned long held = s_btn->heldFor();
  if (held == 0) {
    writeFeedback(false);  // not pressed (or already triggered this cycle)
    return false;
  }

  // Beat cadence: threshold ÷ 8 beats. M1 = single opening blink, M2 = 4
  // escalating flashes, M3 = trigger (handled by LONG_PRESS branch above).
  const uint32_t beat_ms      = s_cfg.threshold_ms / HOLD_BEATS_PER_THRESHOLD;
  const uint32_t half_beat_ms = beat_ms / 2;
  const uint32_t m2_start_ms  = beat_ms * HOLD_BEATS_PER_MEASURE;

  bool led_on;
  if (held < m2_start_ms) {
    led_on = (held < half_beat_ms);  // M1.1 first half only
  } else {
    const uint32_t beat_phase = (held - m2_start_ms) % beat_ms;
    led_on = (beat_phase < half_beat_ms);  // first half of every M2 beat
  }

  writeFeedback(led_on);
  return false;
}
