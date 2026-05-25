#include "LEDSequence.h"

#include <Arduino.h>

#if defined(NRF52_PLATFORM)
  // Use nRF52 SDK headers directly for hardware-timer access.
  #include <nrf.h>
#elif defined(ESP32)
  #include <esp_timer.h>
#endif

// ============================================================================
// Canonical timing constants — single source of truth for all opted-in boards
// ============================================================================

// Boot sequence timings (microseconds for hardware-timer platforms;
// converted to ms via /1000 for synchronous fallback).
static constexpr uint32_t BOOT_BRIGHT_US   = 1000000;  // 1.0 s — both LEDs solid on
static constexpr uint32_t BOOT_DARK1_US    = 1000000;  // 1.0 s — both off
static constexpr uint32_t BOOT_DARK2_US    = 1000000;  // 1.0 s — final gap before BOOT_FLASH
static constexpr uint32_t BOOT_FLASH_US    =  100000;  // 100 ms — closing flash

// FLICKER phase: each tick chooses a random interval in [MIN, MIN+RANGE) µs.
static constexpr uint32_t FLICKER_MIN_US   =   10000;  // 10 ms
static constexpr uint32_t FLICKER_RANGE_US =   90000;  // + 0–90 ms (= 10–100 ms total)

// Safety: cap total flicker ticks so a forgotten onBootComplete() doesn't
// leave the LEDs flickering forever. At ~55 ms avg interval, 300 ticks ≈ 16 s.
static constexpr uint32_t FLICKER_SAFETY_TICKS = 300;

// Power-off cue timings (synchronous, ms units).
static constexpr uint32_t POWEROFF_SOLID_MS      = 1000;
static constexpr uint32_t POWEROFF_DUAL_FLASH_MS =   50;

// Mid-hold power-button feedback proportions (parts-per-1000 of the hold threshold).
// Symmetric: 0-10% flash, 10-50% dark, 50-60% flash, 60-100% dark, 100% commit.
// For a 2000 ms threshold: 0-200, 200-1000, 1000-1200, 1200-2000, commit —
// equal 800 ms dark gaps on each side of FLASH2 for a clean rhythmic feel.
static constexpr uint32_t HOLD_FLASH1_START_PPK =    0;
static constexpr uint32_t HOLD_FLASH1_END_PPK   =  100;
static constexpr uint32_t HOLD_FLASH2_START_PPK =  500;
static constexpr uint32_t HOLD_FLASH2_END_PPK   =  600;

// ============================================================================
// Boot-sequence state machine (driven asynchronously where supported,
// or synchronously by the fallback path)
// ============================================================================

enum BootLedState : uint8_t {
  BOOT_LED_IDLE = 0,
  BOOT_LED_BRIGHT,
  BOOT_LED_DARK1,
  BOOT_LED_FLICKER,
  BOOT_LED_DARK2,
  BOOT_LED_FLASH,
};

// Shared state (volatile because mutated from ISR/timer callback on async platforms)
static LEDSequence::Config       s_cfg               = {};
static bool                      s_enabled           = false;  // true once begin() with valid pins
static volatile BootLedState     s_boot_state        = BOOT_LED_IDLE;
static volatile bool             s_flicker_exit      = false;
static volatile bool             s_flicker_blue_on   = false;
static volatile uint32_t         s_flicker_ticks     = 0;

// xorshift32 PRNG for flicker jitter — fast, deterministic, runs in ISR safely.
static uint32_t s_rng = 0xC0FFEE42;
static inline uint32_t next_random() {
  uint32_t x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rng = x;
  return x;
}

// Hold tracker for pollPowerButton — separate state, separate concerns.
static unsigned long s_hold_down_at = 0;

// ============================================================================
// Low-level LED pin write — handles active-level inversion + null-pin safety
// ============================================================================

static inline void writePin(int8_t pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, on ? s_cfg.active_level : !s_cfg.active_level);
}

static inline void setLEDs(bool primary_on, bool secondary_on) {
  writePin(s_cfg.primary_pin,   primary_on);
  writePin(s_cfg.secondary_pin, secondary_on);
}

// ============================================================================
// Platform-specific timer arm/disarm
// ============================================================================

#if defined(NRF52_PLATFORM)
// --- NRF52: use TIMER2 hardware peripheral with ISR ---

static void arm_timer_us(uint32_t microseconds) {
  NRF_TIMER2->CC[0] = microseconds;
  NRF_TIMER2->TASKS_CLEAR = 1;
}

static void start_timer() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->MODE      = TIMER_MODE_MODE_Timer;
  NRF_TIMER2->BITMODE   = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER2->PRESCALER = 4;  // 16 MHz / 2^4 = 1 MHz tick (1 µs)
  NRF_TIMER2->INTENSET  = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(TIMER2_IRQn, 7);  // low priority
  NVIC_ClearPendingIRQ(TIMER2_IRQn);
  NVIC_EnableIRQ(TIMER2_IRQn);
  NRF_TIMER2->TASKS_START = 1;
}

static void stop_timer() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->INTENCLR   = TIMER_INTENCLR_COMPARE0_Msk;
  NVIC_DisableIRQ(TIMER2_IRQn);
}

#elif defined(ESP32)
// --- ESP32: use esp_timer (FreeRTOS-task-backed callbacks, no ISR restrictions) ---

static esp_timer_handle_t s_esp_timer = nullptr;
static void esp_timer_callback(void* arg);  // forward

static void arm_timer_us(uint32_t microseconds) {
  if (s_esp_timer) {
    esp_timer_stop(s_esp_timer);
    esp_timer_start_once(s_esp_timer, microseconds);
  }
}

static void start_timer() {
  if (!s_esp_timer) {
    esp_timer_create_args_t args = {};
    args.callback = &esp_timer_callback;
    args.name     = "LEDSequence";
    esp_timer_create(&args, &s_esp_timer);
  }
}

static void stop_timer() {
  if (s_esp_timer) {
    esp_timer_stop(s_esp_timer);
  }
}

#else
// --- Fallback: no async timer. Timer arm/start/stop are all no-ops. ---
static inline void arm_timer_us(uint32_t) {}
static inline void start_timer()          {}
static inline void stop_timer()           {}
#endif

// ============================================================================
// Boot state machine advance — platform-agnostic core called from ISR/callback
// ============================================================================

// Single state transition. Sets LEDs, decides next interval, arms timer.
// On the IDLE / FLASH→done transition, also stops the timer entirely.
static void advance_boot_state() {
  switch (s_boot_state) {
    case BOOT_LED_BRIGHT:
      // Exit BRIGHT (both LEDs were HIGH from playBoot). Enter DARK1.
      setLEDs(false, false);
      s_boot_state = BOOT_LED_DARK1;
      arm_timer_us(BOOT_DARK1_US);
      break;

    case BOOT_LED_DARK1:
      // Enter FLICKER: primary solid on, secondary will toggle.
      writePin(s_cfg.primary_pin, true);
      s_flicker_blue_on = false;
      s_flicker_ticks   = 0;
      s_boot_state      = BOOT_LED_FLICKER;
      arm_timer_us(FLICKER_MIN_US + (next_random() % FLICKER_RANGE_US));
      break;

    case BOOT_LED_FLICKER:
      // Check exit conditions BEFORE toggling so a fast onBootComplete()
      // shortcuts the flicker cleanly.
      s_flicker_ticks++;
      if (s_flicker_exit || s_flicker_ticks > FLICKER_SAFETY_TICKS) {
        // Enter DARK2.
        setLEDs(false, false);
        s_boot_state = BOOT_LED_DARK2;
        arm_timer_us(BOOT_DARK2_US);
      } else {
        // Continue flicker: toggle secondary (or primary if no secondary).
        s_flicker_blue_on = !s_flicker_blue_on;
        int8_t flick_pin = (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin
                                                       : s_cfg.primary_pin;
        writePin(flick_pin, s_flicker_blue_on);
        arm_timer_us(FLICKER_MIN_US + (next_random() % FLICKER_RANGE_US));
      }
      break;

    case BOOT_LED_DARK2:
      // Enter FLASH: brief secondary (or primary fallback) closing flash.
      {
        int8_t flash_pin = (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin
                                                       : s_cfg.primary_pin;
        writePin(flash_pin, true);
      }
      s_boot_state = BOOT_LED_FLASH;
      arm_timer_us(BOOT_FLASH_US);
      break;

    case BOOT_LED_FLASH:
      // Enter IDLE: LEDs off, stop the timer.
      setLEDs(false, false);
      s_boot_state = BOOT_LED_IDLE;
      stop_timer();
      break;

    case BOOT_LED_IDLE:
    default:
      // Safety net — shouldn't happen but ensure the timer stops.
      stop_timer();
      break;
  }
}

// ============================================================================
// Platform-specific timer entry points → call into advance_boot_state()
// ============================================================================

#if defined(NRF52_PLATFORM)
extern "C" void TIMER2_IRQHandler(void) {
  if (!NRF_TIMER2->EVENTS_COMPARE[0]) return;
  NRF_TIMER2->EVENTS_COMPARE[0] = 0;
  NRF_TIMER2->TASKS_CLEAR = 1;
  advance_boot_state();
}
#elif defined(ESP32)
static void esp_timer_callback(void* /*arg*/) {
  advance_boot_state();
}
#endif

// ============================================================================
// Public API
// ============================================================================

void LEDSequence::begin(const LEDSequence::Config& cfg) {
  s_cfg = cfg;
  // "Enabled" means we have at least a primary LED OR a buzzer to drive.
  // Boards with no feedback hardware get all no-op behavior.
  s_enabled = (cfg.primary_pin >= 0) || (cfg.buzzer_pin >= 0);

  if (cfg.primary_pin >= 0) {
    pinMode(cfg.primary_pin, OUTPUT);
    writePin(cfg.primary_pin, false);
  }
  if (cfg.secondary_pin >= 0) {
    pinMode(cfg.secondary_pin, OUTPUT);
    writePin(cfg.secondary_pin, false);
  }
  // Buzzer pin init is deferred to the buzzer integration — left as a
  // future extension since none of the initial opt-in boards have buzzers
  // wired through this helper yet.

  s_boot_state    = BOOT_LED_IDLE;
  s_flicker_exit  = false;
}

void LEDSequence::playBoot() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  // Async path: set initial BRIGHT state, arm timer, return immediately.
  // The state machine runs in the background via interrupts/callbacks while
  // setup() continues with radio/SPI/BLE init in parallel.
  s_flicker_exit  = false;
  s_flicker_ticks = 0;
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;
  start_timer();
  arm_timer_us(BOOT_BRIGHT_US);
#else
  // Fallback path: no hardware timer available. Set LEDs ON statically
  // and return immediately. onBootComplete() will turn them off + flash.
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;  // tracked so onBootComplete knows
#endif
}

void LEDSequence::onBootComplete() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  // Async path: just signal the FLICKER state to exit on its next tick.
  // The remaining DARK2 + FLASH + off phases play out in the background.
  s_flicker_exit = true;
#else
  // Fallback path: do a synchronous final flash and turn LEDs off.
  if (s_boot_state == BOOT_LED_IDLE) return;
  setLEDs(false, false);
  delay(BOOT_DARK2_US / 1000);
  int8_t flash_pin = (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin
                                                 : s_cfg.primary_pin;
  writePin(flash_pin, true);
  delay(BOOT_FLASH_US / 1000);
  setLEDs(false, false);
  s_boot_state = BOOT_LED_IDLE;
#endif
}

void LEDSequence::cancel() {
  if (!s_enabled) return;
  stop_timer();
  setLEDs(false, false);
  s_boot_state    = BOOT_LED_IDLE;
  s_flicker_exit  = false;
  s_flicker_ticks = 0;
}

void LEDSequence::playPowerOff() {
  // Power-off cue is always synchronous — there's no reason to be async
  // here (the device is about to sleep anyway).
  if (!s_enabled) return;

  // Make sure the boot sequence isn't still running and won't race with us.
  cancel();

  // SOLID: primary full bright for 1 second.
  writePin(s_cfg.primary_pin,   true);
  writePin(s_cfg.secondary_pin, false);
  delay(POWEROFF_SOLID_MS);

  // DUAL_FLASH: both LEDs (or just primary on 1-LED boards) for 50 ms.
  writePin(s_cfg.primary_pin,   true);
  writePin(s_cfg.secondary_pin, true);
  delay(POWEROFF_DUAL_FLASH_MS);

  // Off.
  setLEDs(false, false);

  // TODO: buzzer descending shutdown tone (when buzzer integration lands).
}

bool LEDSequence::pollPowerButton(int8_t pin, uint32_t threshold_ms) {
  if (pin < 0) return false;

  int btnState = digitalRead(pin);
  if (btnState != LOW) {
    // Button released. Clear feedback LEDs if we were tracking a hold.
    if (s_hold_down_at != 0) {
      setLEDs(false, false);
      s_hold_down_at = 0;
    }
    return false;
  }

  // Button is pressed (assumes active-low — same convention as M6/SenseCAP).
  if (s_hold_down_at == 0) {
    s_hold_down_at = millis();
  }
  unsigned long held = millis() - s_hold_down_at;

  // Commit?
  if (held >= threshold_ms) {
    // Caller will invoke powerOff(). Reset hold tracker so a quick release
    // after powerOff() doesn't immediately re-fire.
    s_hold_down_at = 0;
    return true;
  }

  // Progressive feedback. Compute phase based on parts-per-1000 of threshold.
  // (Multiply before divide to keep precision with integer math.)
  uint32_t ppk = (uint32_t)((held * 1000ULL) / threshold_ms);

  bool in_flash1 = (ppk >= HOLD_FLASH1_START_PPK && ppk < HOLD_FLASH1_END_PPK);
  bool in_flash2 = (ppk >= HOLD_FLASH2_START_PPK && ppk < HOLD_FLASH2_END_PPK);

  if (in_flash1 || in_flash2) {
    writePin(s_cfg.primary_pin,   true);
    writePin(s_cfg.secondary_pin, false);
  } else {
    setLEDs(false, false);
  }

  return false;
}
