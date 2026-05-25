#include "HoldButton.h"

// Beat structure for the mid-hold cadence. See header for the visual spec.
static constexpr uint32_t HOLD_BEATS_PER_THRESHOLD = 8;
static constexpr uint32_t HOLD_BEATS_PER_MEASURE   = 4;

// Configuration + hold-tracking state. Singleton (one HoldButton per board)
// — see header notes on the rationale.
static HoldButton::Config s_cfg          = {};
static unsigned long       s_hold_down_at = 0;

static inline void writeFeedback(bool on) {
  if (s_cfg.feedback_pin < 0) return;
  digitalWrite(s_cfg.feedback_pin, on ? s_cfg.active_level : !s_cfg.active_level);
}

void HoldButton::begin(const HoldButton::Config& cfg) {
  s_cfg          = cfg;
  s_hold_down_at = 0;
  // Note: button pinMode is the board's responsibility (usually INPUT_PULLUP
  // inside the board's begin()). Feedback pin is typically already set as
  // OUTPUT by whatever owns the LED — we just write to it.
}

bool HoldButton::poll() {
  if (s_cfg.pin < 0) return false;

  int btnState = digitalRead(s_cfg.pin);
  if (btnState != LOW) {
    // Button released. Clear feedback and reset tracker if we were holding.
    if (s_hold_down_at != 0) {
      writeFeedback(false);
      s_hold_down_at = 0;
    }
    return false;
  }

  // Button pressed (active-low).
  if (s_hold_down_at == 0) {
    s_hold_down_at = millis();
  }
  unsigned long held = (unsigned long)(millis() - s_hold_down_at);

  // Commit?
  if (held >= s_cfg.threshold_ms) {
    // Reset so a quick post-powerOff release doesn't immediately re-fire
    // (also, powerOff() typically doesn't return — but be safe).
    s_hold_down_at = 0;
    return true;
  }

  // Beat-based feedback. Threshold splits into 8 beats (2 measures × 4
  // beats); measure 1 is a single opening blink, measure 2 is 4 escalating
  // flashes, measure 3 is the commit (handled by the caller via powerOff).
  const uint32_t beat_ms        = s_cfg.threshold_ms / HOLD_BEATS_PER_THRESHOLD;
  const uint32_t half_beat_ms   = beat_ms / 2;
  const uint32_t m2_start_ms    = beat_ms * HOLD_BEATS_PER_MEASURE;  // = threshold/2

  bool led_on;
  if (held < m2_start_ms) {
    // Measure 1: only the first half of beat 1 is on.
    led_on = (held < half_beat_ms);
  } else {
    // Measure 2: first half of every beat is on (4 escalating flashes).
    const uint32_t beat_phase = (held - m2_start_ms) % beat_ms;
    led_on = (beat_phase < half_beat_ms);
  }

  writeFeedback(led_on);
  return false;
}
