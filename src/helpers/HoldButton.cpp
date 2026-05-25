#include "HoldButton.h"
#include <helpers/ui/MomentaryButton.h>

// Beat structure for the mid-hold cadence. See header for the visual spec.
static constexpr uint32_t HOLD_BEATS_PER_THRESHOLD = 8;
static constexpr uint32_t HOLD_BEATS_PER_MEASURE   = 4;

// Configuration + button instance. Singleton (one HoldButton per board) —
// see header notes on the rationale.
//
// MomentaryButton is heap-allocated in begin() so its constructor params
// (pin, threshold) can be set from the Config struct rather than fixed at
// static-init time. One allocation per boot; never freed (begin() is
// expected to be called exactly once per device lifetime).
static HoldButton::Config s_cfg = {};
static MomentaryButton*   s_btn = nullptr;

static inline void writeFeedback(bool on) {
  if (s_cfg.feedback_pin < 0) return;
  digitalWrite(s_cfg.feedback_pin, on ? s_cfg.active_level : !s_cfg.active_level);
}

void HoldButton::begin(const HoldButton::Config& cfg) {
  s_cfg = cfg;

  if (s_btn) {
    delete s_btn;
    s_btn = nullptr;
  }
  if (cfg.pin < 0) return;  // no button configured — poll() will no-op

  // reverse=true: active-low (button pulls line LOW when pressed).
  // pulldownup=true: configure INPUT_PULLUP via MomentaryButton's begin().
  //   Redundant with any earlier pinMode() the board did, but harmless.
  // multiclick=false: we don't care about click count — just long-press +
  //   the mid-hold heldFor() accessor for our beat feedback.
  s_btn = new MomentaryButton(cfg.pin, (int)cfg.threshold_ms, true, true, false);
  s_btn->begin();
}

bool HoldButton::poll() {
  if (!s_btn) return false;

  // Drive MomentaryButton — this is what updates its internal down_at
  // timestamp that heldFor() reads. Returns BUTTON_EVENT_LONG_PRESS at
  // exactly the moment the hold threshold is reached.
  const int ev = s_btn->check();

  if (ev == BUTTON_EVENT_LONG_PRESS) {
    // Trigger. Clear feedback LED and let caller invoke whatever action
    // (typically board.powerOff(), but the caller decides).
    writeFeedback(false);
    return true;
  }

  // Mid-hold feedback. heldFor() returns 0 if the button isn't currently
  // pressed (or after long-press has fired), and the elapsed millis since
  // press otherwise.
  const unsigned long held = s_btn->heldFor();
  if (held == 0) {
    writeFeedback(false);
    return false;
  }

  // Beat-based cadence: threshold splits into 8 beats (2 measures × 4
  // beats); measure 1 is a single opening blink, measure 2 is 4 escalating
  // flashes, measure 3 is the commit (handled by the long-press branch
  // above and caller's response to our `true` return).
  const uint32_t beat_ms      = s_cfg.threshold_ms / HOLD_BEATS_PER_THRESHOLD;
  const uint32_t half_beat_ms = beat_ms / 2;
  const uint32_t m2_start_ms  = beat_ms * HOLD_BEATS_PER_MEASURE;  // = threshold/2

  bool led_on;
  if (held < m2_start_ms) {
    // Measure 1: only beat 1's first half is on (single opening blink).
    led_on = (held < half_beat_ms);
  } else {
    // Measure 2: first half of every beat is on (4 escalating flashes).
    const uint32_t beat_phase = (held - m2_start_ms) % beat_ms;
    led_on = (beat_phase < half_beat_ms);
  }

  writeFeedback(led_on);
  return false;
}
