#include "LEDSequence.h"

#include <Arduino.h>

#if defined(NRF52_PLATFORM)
  #include <nrf.h>
#elif defined(ESP32)
  #include <esp_timer.h>
#endif

static constexpr uint32_t BOOT_BRIGHT_US         = 1000000;  // 1.0s both LEDs on
static constexpr uint32_t BOOT_FLASH_US          =  100000;  // 100ms closing flash
static constexpr uint32_t POWEROFF_SOLID_MS      =    1000;
static constexpr uint32_t POWEROFF_DUAL_FLASH_MS =      50;

enum BootLedState : uint8_t {
  BOOT_LED_IDLE = 0,
  BOOT_LED_BRIGHT,         // both LEDs on, BRIGHT timer running
  BOOT_LED_WAIT_FOR_BOOT,  // BRIGHT done, waiting for onBootComplete()
  BOOT_LED_FLASH,          // closing flash, FLASH timer running
};

// volatile: mutated from ISR/timer callback on async platforms
static LEDSequence::Config   s_cfg              = {};
static bool                  s_enabled          = false;
static volatile BootLedState s_boot_state       = BOOT_LED_IDLE;
static volatile bool         s_flash_requested  = false;  // onBootComplete fired during BRIGHT

static inline void writePin(int8_t pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, on ? s_cfg.active_level : !s_cfg.active_level);
}

static inline void setLEDs(bool primary_on, bool secondary_on) {
  writePin(s_cfg.primary_pin,   primary_on);
  writePin(s_cfg.secondary_pin, secondary_on);
}

static inline int8_t flashPin() {
  return (s_cfg.secondary_pin >= 0) ? s_cfg.secondary_pin : s_cfg.primary_pin;
}

#if defined(NRF52_PLATFORM)
// NRF52: TIMER2 hardware peripheral with ISR
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
// ESP32: esp_timer (FreeRTOS-task-backed; no ISR restrictions)
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
      if (s_flash_requested) {
        // onBootComplete fired during BRIGHT — go straight to FLASH
        writePin(flashPin(), true);
        s_boot_state = BOOT_LED_FLASH;
        arm_timer_us(BOOT_FLASH_US);
      } else {
        // BRIGHT ended but boot still running; wait for onBootComplete()
        s_boot_state = BOOT_LED_WAIT_FOR_BOOT;
        stop_timer();
      }
      break;

    case BOOT_LED_FLASH:
      setLEDs(false, false);
      s_boot_state = BOOT_LED_IDLE;
      stop_timer();
      break;

    case BOOT_LED_IDLE:
    case BOOT_LED_WAIT_FOR_BOOT:
    default:
      stop_timer();  // safety
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

  s_boot_state      = BOOT_LED_IDLE;
  s_flash_requested = false;
}

void LEDSequence::playBoot() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  s_flash_requested = false;
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;
  start_timer();
  arm_timer_us(BOOT_BRIGHT_US);
#else
  // No hardware timer: LEDs stay on until onBootComplete() runs the
  // synchronous flash sequence.
  setLEDs(true, true);
  s_boot_state = BOOT_LED_BRIGHT;
#endif
}

void LEDSequence::onBootComplete() {
  if (!s_enabled) return;

#if defined(NRF52_PLATFORM) || defined(ESP32)
  if (s_boot_state == BOOT_LED_BRIGHT) {
    // BRIGHT still running — flag it; BRIGHT-end ISR runs the flash directly
    s_flash_requested = true;
  } else if (s_boot_state == BOOT_LED_WAIT_FOR_BOOT) {
    // BRIGHT done; trigger FLASH now
    writePin(flashPin(), true);
    s_boot_state = BOOT_LED_FLASH;
    arm_timer_us(BOOT_FLASH_US);
  }
  // else IDLE / FLASH: already done or in progress
#else
  // Fallback: synchronous close
  if (s_boot_state == BOOT_LED_IDLE) return;
  setLEDs(false, false);
  writePin(flashPin(), true);
  delay(BOOT_FLASH_US / 1000);
  setLEDs(false, false);
  s_boot_state = BOOT_LED_IDLE;
#endif
}

void LEDSequence::cancel() {
  if (!s_enabled) return;
  stop_timer();
  setLEDs(false, false);
  s_boot_state      = BOOT_LED_IDLE;
  s_flash_requested = false;
}

void LEDSequence::playPowerOff() {
  if (!s_enabled) return;
  cancel();  // stop boot sequence before taking over the pins

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
