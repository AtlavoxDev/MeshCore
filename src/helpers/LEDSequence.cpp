#include "LEDSequence.h"

#include <Arduino.h>

#if defined(NRF52_PLATFORM)
  #include <nrf.h>
#elif defined(ESP32)
  #include <esp_timer.h>
#endif

// Timing constants (microseconds for hardware-timer platforms).
static constexpr uint32_t BOOT_BRIGHT_US       = 1000000;  // 1.0s both LEDs on
static constexpr uint32_t BOOT_DARK1_US        = 1000000;  // 1.0s gap before flicker
static constexpr uint32_t BOOT_DARK2_US        = 1000000;  // 1.0s gap before final flash
static constexpr uint32_t BOOT_FLASH_US        =  100000;  // 100ms closing flash
static constexpr uint32_t FLICKER_MIN_US       =   10000;  // 10ms min interval
static constexpr uint32_t FLICKER_RANGE_US     =   90000;  // +0-90ms jitter
static constexpr uint32_t FLICKER_MIN_TICKS    =      10;  // always flicker ≥ ~550ms even on fast boot
static constexpr uint32_t FLICKER_SAFETY_TICKS =     300;  // ~16s fallback cap
static constexpr uint32_t POWEROFF_SOLID_MS    =    1000;
static constexpr uint32_t POWEROFF_DUAL_FLASH_MS =    50;

enum BootLedState : uint8_t {
  BOOT_LED_IDLE = 0,
  BOOT_LED_BRIGHT,
  BOOT_LED_DARK1,
  BOOT_LED_FLICKER,
  BOOT_LED_DARK2,
  BOOT_LED_FLASH,
};

// volatile: mutated from ISR/timer callback on async platforms
static LEDSequence::Config   s_cfg             = {};
static bool                  s_enabled         = false;
static volatile BootLedState s_boot_state      = BOOT_LED_IDLE;
static volatile bool         s_flicker_exit    = false;
static volatile bool         s_flicker_blue_on = false;
static volatile uint32_t     s_flicker_ticks   = 0;

// xorshift32 PRNG — ISR-safe (no malloc, no globals beyond s_rng).
static uint32_t s_rng = 0xC0FFEE42;
static inline uint32_t next_random() {
  uint32_t x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rng = x;
  return x;
}

static inline void writePin(int8_t pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, on ? s_cfg.active_level : !s_cfg.active_level);
}

static inline void setLEDs(bool primary_on, bool secondary_on) {
  writePin(s_cfg.primary_pin,   primary_on);
  writePin(s_cfg.secondary_pin, secondary_on);
}

#if defined(NRF52_PLATFORM)
// NRF52: use TIMER2 hardware peripheral with ISR
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
  NVIC_SetPriority(TIMER2_IRQn, 7);
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
// ESP32: use esp_timer (FreeRTOS-task-backed; no ISR restrictions)
static esp_timer_handle_t s_esp_timer = nullptr;
static void esp_timer_callback(void* arg);
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
  if (s_esp_timer) esp_timer_stop(s_esp_timer);
}
#else
// Fallback: no hardware timer
static inline void arm_timer_us(uint32_t) {}
static inline void start_timer()          {}
static inline void stop_timer()           {}
#endif

// Single state transition. Called from ISR/timer callback.
static void advance_boot_state() {
  switch (s_boot_state) {
    case BOOT_LED_BRIGHT:
      setLEDs(false, false);
      s_boot_state = BOOT_LED_DARK1;
      arm_timer_us(BOOT_DARK1_US);
      break;

    case BOOT_LED_DARK1:
      writePin(s_cfg.primary_pin, true);  // primary solid for flicker phase
      s_flicker_blue_on = false;
      s_flicker_ticks   = 0;
      s_boot_state      = BOOT_LED_FLICKER;
      arm_timer_us(FLICKER_MIN_US + (next_random() % FLICKER_RANGE_US));
      break;

    case BOOT_LED_FLICKER:
      // Check exit BEFORE toggling so onBootComplete() shortcuts cleanly.
      // Require a minimum tick count so fast-boot devices (whose setup()
      // finishes before BRIGHT+DARK1 elapse) still get a visible flicker
      // instead of immediately bailing on the first tick.
      s_flicker_ticks++;
      if ((s_flicker_exit && s_flicker_ticks >= FLICKER_MIN_TICKS) ||
           s_flicker_ticks > FLICKER_SAFETY_TICKS) {
        setLEDs(false, false);
        s_boot_state = BOOT_LED_DARK2;
        arm_timer_us(BOOT_DARK2_US);
      } else {
        s_flicker_blue_on = !s_flicker_blue_on;
        int8_t flick_pin = (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin
                                                       : s_cfg.primary_pin;
        writePin(flick_pin, s_flicker_blue_on);
        arm_timer_us(FLICKER_MIN_US + (next_random() % FLICKER_RANGE_US));
      }
      break;

    case BOOT_LED_DARK2: {
      int8_t flash_pin = (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin
                                                     : s_cfg.primary_pin;
      writePin(flash_pin, true);
      s_boot_state = BOOT_LED_FLASH;
      arm_timer_us(BOOT_FLASH_US);
      break;
    }

    case BOOT_LED_FLASH:
      setLEDs(false, false);
      s_boot_state = BOOT_LED_IDLE;
      stop_timer();
      break;

    case BOOT_LED_IDLE:
    default:
      stop_timer();  // safety net
      break;
  }
}

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

void LEDSequence::begin(const LEDSequence::Config& cfg) {
  s_cfg     = cfg;
  s_enabled = (cfg.primary_pin >= 0) || (cfg.buzzer_pin >= 0);

  if (cfg.primary_pin >= 0) {
    pinMode(cfg.primary_pin, OUTPUT);
    writePin(cfg.primary_pin, false);
  }
  if (cfg.secondary_pin >= 0) {
    pinMode(cfg.secondary_pin, OUTPUT);
    writePin(cfg.secondary_pin, false);
  }
  // TODO: buzzer pin init when buzzer integration lands

  s_boot_state   = BOOT_LED_IDLE;
  s_flicker_exit = false;
}

void LEDSequence::playBoot() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  s_flicker_exit  = false;
  s_flicker_ticks = 0;
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;
  start_timer();
  arm_timer_us(BOOT_BRIGHT_US);
#else
  // No hardware timer: LEDs stay on until onBootComplete().
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;
#endif
}

void LEDSequence::onBootComplete() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  s_flicker_exit = true;  // ISR sees this on next tick and runs DARK2 + FLASH
#else
  // Sync fallback: final flash, then off.
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
  if (!s_enabled) return;
  cancel();  // stop any running boot sequence before we take over the pins

  // SOLID: primary on, secondary off, 1s
  writePin(s_cfg.primary_pin,   true);
  writePin(s_cfg.secondary_pin, false);
  delay(POWEROFF_SOLID_MS);

  // DUAL_FLASH: both on, 50ms
  writePin(s_cfg.primary_pin,   true);
  writePin(s_cfg.secondary_pin, true);
  delay(POWEROFF_DUAL_FLASH_MS);

  setLEDs(false, false);
  // TODO: buzzer descending shutdown tone
}
