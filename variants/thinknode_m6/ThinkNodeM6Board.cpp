#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>

// Function-button timing for hold-to-power-off.
//   Brief red blink at 0 s and 1 s during the hold, then board.powerOff()
//   runs the final cue when held past 2 s.
#define M6_OFF_FLASH1_START_MS  0
#define M6_OFF_FLASH1_END_MS    200
#define M6_OFF_FLASH2_START_MS  1000
#define M6_OFF_FLASH2_END_MS    1200
#define M6_OFF_COMMIT_MS        2000
#define M6_OFF_FLASH_BRIGHT     128   // ~50% of 255

// Boot LED state machine — fully async, driven by the TIMER2 ISR.
// begin() kicks it off and returns immediately; setup() proceeds in parallel
// with the LED choreography. bootComplete() signals the FLICKER state to
// exit and advance toward DONE. A safety tick counter ensures the sequence
// completes even if bootComplete() is never called.
enum BootLedState : uint8_t {
  BOOT_LED_IDLE = 0,
  BOOT_LED_BOTH_BRIGHT,
  BOOT_LED_DARK1,
  BOOT_LED_FLICKER,
  BOOT_LED_DARK2,
  BOOT_LED_BOOT_FLASH,
};

static volatile BootLedState s_boot_state = BOOT_LED_IDLE;
static volatile bool         s_flicker_exit_requested = false;
static volatile bool         s_flicker_blue_on        = false;
static volatile uint32_t     s_flicker_ticks          = 0;
#define M6_FLICKER_SAFETY_TICKS  300  // ~15 s fallback at ~50 ms avg per tick

static uint32_t s_flicker_rng = 0xC0FFEE42;
// xorshift32 PRNG for flicker jitter.
static inline uint32_t flicker_next_rand() {
  uint32_t x = s_flicker_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_flicker_rng = x;
  return x;
}

extern "C" void TIMER2_IRQHandler(void) {
  if (!NRF_TIMER2->EVENTS_COMPARE[0]) return;
  NRF_TIMER2->EVENTS_COMPARE[0] = 0;
  NRF_TIMER2->TASKS_CLEAR = 1;

  switch (s_boot_state) {
    case BOOT_LED_BOTH_BRIGHT:
      // Exit BOTH_BRIGHT (LEDs were set HIGH by startBootSequence). Enter DARK1.
      nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_RED],  0);
      nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], 0);
      s_boot_state = BOOT_LED_DARK1;
      NRF_TIMER2->CC[0] = 1000000;  // 1 s
      break;

    case BOOT_LED_DARK1:
      // Enter FLICKER: red solid on (the non-TX "power" LED).
      nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_RED], 1);
      s_flicker_blue_on = false;
      s_flicker_ticks   = 0;
      s_boot_state = BOOT_LED_FLICKER;
      NRF_TIMER2->CC[0] = 10000 + (flicker_next_rand() % 90000);  // first blue toggle
      break;

    case BOOT_LED_FLICKER:
      // Check exit conditions BEFORE toggling so a fast bootComplete() can
      // shortcut the flicker cleanly.
      s_flicker_ticks++;
      if (s_flicker_exit_requested || s_flicker_ticks > M6_FLICKER_SAFETY_TICKS) {
        // Enter DARK2.
        nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_RED],  0);
        nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], 0);
        s_boot_state = BOOT_LED_DARK2;
        NRF_TIMER2->CC[0] = 1000000;  // 1 s gap before final flash
      } else {
        // Continue flicker: toggle blue (the TX LED), schedule next tick.
        s_flicker_blue_on = !s_flicker_blue_on;
        nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], s_flicker_blue_on ? 1 : 0);
        NRF_TIMER2->CC[0] = 10000 + (flicker_next_rand() % 90000);
      }
      break;

    case BOOT_LED_DARK2:
      // Enter BOOT_FLASH: brief blue flash to signal boot complete.
      nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], 1);
      s_boot_state = BOOT_LED_BOOT_FLASH;
      NRF_TIMER2->CC[0] = 100000;  // 100 ms
      break;

    case BOOT_LED_BOOT_FLASH:
      // Enter DONE: LEDs off, stop the timer.
      nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], 0);
      s_boot_state = BOOT_LED_IDLE;
      NRF_TIMER2->TASKS_STOP = 1;
      NRF_TIMER2->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
      NVIC_DisableIRQ(TIMER2_IRQn);
      break;

    case BOOT_LED_IDLE:
    default:
      // Should not happen — safety net.
      NRF_TIMER2->TASKS_STOP = 1;
      NRF_TIMER2->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
      NVIC_DisableIRQ(TIMER2_IRQn);
      break;
  }
}

// Hard-stop the boot LED state machine. Used by powerOff() to prevent the
// TIMER2 ISR from racing with the synchronous shutdown cue's analogWrite()s.
static void stopBootSequence() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
  NVIC_DisableIRQ(TIMER2_IRQn);
  s_boot_state = BOOT_LED_IDLE;
}

// Kick off the boot LED state machine. Non-blocking — returns immediately.
// Sets LEDs to the BOTH_BRIGHT initial state and configures TIMER2 to advance
// the state machine in the background while setup() continues.
static void startBootSequence() {
  s_flicker_exit_requested = false;
  s_flicker_blue_on        = false;
  s_flicker_ticks          = 0;

  // Initial visible state: both LEDs full bright.
  digitalWrite(PIN_LED_RED,  HIGH);
  digitalWrite(PIN_LED_BLUE, HIGH);
  s_boot_state = BOOT_LED_BOTH_BRIGHT;

  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER2->PRESCALER = 4;  // 16 MHz / 2^4 = 1 MHz tick (1 µs)
  NRF_TIMER2->CC[0] = 1000000;  // BOTH_BRIGHT lasts 1 s, then ISR advances
  NRF_TIMER2->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(TIMER2_IRQn, 7);  // low priority
  NVIC_ClearPendingIRQ(TIMER2_IRQn);
  NVIC_EnableIRQ(TIMER2_IRQn);
  NRF_TIMER2->TASKS_CLEAR = 1;
  NRF_TIMER2->TASKS_START = 1;
}

// Arm the Function Button as a SENSE-LOW wake source and enter SYSTEMOFF.
// Falls back to a direct register write if SoftDevice isn't enabled
// (non-BLE builds).
static void enterDeepSleep() {
  nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_USER_BTN]),
                           NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    if (sd_power_system_off() == NRF_ERROR_SOFTDEVICE_NOT_ENABLED) {
      sd_enabled = 0;
    }
  }
  if (!sd_enabled) {
    NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
  }
  NVIC_SystemReset();  // unreachable
}

// Captured by variant.cpp's early constructor. See that file for details.
extern volatile uint32_t g_m6_reset_reason;
extern volatile bool     g_m6_was_shutdown;

void ThinkNodeM6Board::begin() {
  NRF52Board::begin();

  // The boot sequence drives the LEDs via digitalWrite throughout.
  // analogWrite() must not be called on these pins before powerOff(),
  // because on the Adafruit nRF52 core it routes the pin to the PWM
  // peripheral and subsequent digitalWrite() calls no longer drive it.
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  pinMode(PIN_LED_RED,  OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
  delay(20);  // pin settle / debounce

  // Boot decision:
  //   g_m6_was_shutdown && no button wake => the user deliberately powered
  //     off and isn't asking to come back. Stay asleep.
  //   otherwise => boot (fresh power-up, dead-battery recovery, transient
  //     reset of a running device, reset pin, or Function-Button wake).
  bool from_reset_pin   = (g_m6_reset_reason & POWER_RESETREAS_RESETPIN_Msk) != 0;
  bool from_button_wake = (g_m6_reset_reason & POWER_RESETREAS_OFF_Msk)      != 0;

  if (g_m6_was_shutdown && !from_reset_pin && !from_button_wake) {
    enterDeepSleep();
  }

  // Clear the user-intent flag now that we've committed to booting, so the
  // next reset starts from a clean "I'm running" state.
  NRF_POWER->GPREGRET2 = 0;

  // Kick off the boot LED state machine. Non-blocking — TIMER2 drives the
  // entire sequence (bright → dark → solid + flicker → dark → flash) in the
  // background while setup() proceeds. bootComplete() ends the FLICKER phase.
  startBootSequence();

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  delay(10); // give sx1262 some time to power up
}

void ThinkNodeM6Board::powerOff() {
  // Make sure the boot LED state machine isn't still running and won't race
  // with the analogWrite() calls below.
  stopBootSequence();

#ifdef P_LORA_TX_LED
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  // Shutdown cue: red full bright for 1 s, then a brief both-LED flash.
  analogWrite(PIN_LED_BLUE, 0);
  analogWrite(PIN_LED_RED,  255);
  delay(1000);
  analogWrite(PIN_LED_RED, 0);
  analogWrite(PIN_LED_RED,  255);
  analogWrite(PIN_LED_BLUE, 255);
  delay(50);
  analogWrite(PIN_LED_RED,  0);
  analogWrite(PIN_LED_BLUE, 0);

  // SENSE-LOW would fire immediately if we enter SYSTEMOFF with the button
  // still held — wait for release first.
  while (digitalRead(PIN_USER_BTN) == LOW) delay(10);

  Serial.flush();
  delay(50);

  // User-intent magic byte; read by variant.cpp's early constructor.
  NRF_POWER->GPREGRET2 = 0xA5;

  enterDeepSleep();
}

uint16_t ThinkNodeM6Board::getBattMilliVolts() {
  int adcvalue = 0;

  digitalWrite(PIN_ADC_CTRL, HIGH);
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(10);

  // ADC range is 0..3000mV and resolution is 12-bit (0..4095)
  adcvalue = analogRead(PIN_VBAT_READ);
  digitalWrite(PIN_ADC_CTRL, LOW);
  // Convert the raw value to compensated mv, taking the resistor-
  // divider into account (providing the actual LIPO voltage)
  return (uint16_t)((float)adcvalue * REAL_VBAT_MV_PER_LSB);
}

void ThinkNodeM6Board::bootComplete() {
  // Signal the TIMER2 state machine to exit the FLICKER state. The ISR
  // handles the rest of the sequence (dark 1 s gap + 100 ms blue flash + off).
  // Returns immediately — the visual continues in the background.
  s_flicker_exit_requested = true;
}

void ThinkNodeM6Board::pollButton() {
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (_btn_down_at == 0) {
      _btn_down_at = millis();
    }
    unsigned long held = millis() - _btn_down_at;

    if (held >= M6_OFF_COMMIT_MS) {
      Serial.println("Powering off...");
      powerOff();  // does not return
    } else if ((held >= M6_OFF_FLASH1_START_MS && held < M6_OFF_FLASH1_END_MS) ||
               (held >= M6_OFF_FLASH2_START_MS && held < M6_OFF_FLASH2_END_MS)) {
      analogWrite(PIN_LED_RED,    M6_OFF_FLASH_BRIGHT);
      digitalWrite(PIN_LED_BLUE,  LOW);
    } else {
      analogWrite(PIN_LED_RED,    0);
      digitalWrite(PIN_LED_BLUE,  LOW);
    }
  } else {
    // Button released before commit — clear LEDs and reset state.
    if (_btn_down_at != 0) {
      analogWrite(PIN_LED_RED,    0);
      digitalWrite(PIN_LED_BLUE,  LOW);
    }
    _btn_down_at = 0;
  }
}

#endif
