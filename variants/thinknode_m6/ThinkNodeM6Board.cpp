#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>

// Boot-phase blue LED flicker. TIMER2 fires at pseudo-random 10-100 ms
// intervals; the ISR toggles the blue LED. Runs in the background through
// every blocking call in setup() (mesh init, etc.).
static volatile bool s_flicker_blue_on = false;
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
  if (NRF_TIMER2->EVENTS_COMPARE[0]) {
    NRF_TIMER2->EVENTS_COMPARE[0] = 0;
    NRF_TIMER2->TASKS_CLEAR = 1;

    s_flicker_blue_on = !s_flicker_blue_on;
    nrf_gpio_pin_write(g_ADigitalPinMap[PIN_LED_BLUE], s_flicker_blue_on ? 1 : 0);

    NRF_TIMER2->CC[0] = 10000 + (flicker_next_rand() % 90000);  // 10-100 ms
  }
}

static void startBootFlicker() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER2->PRESCALER = 4;  // 16 MHz / 2^4 = 1 MHz tick (1 µs)
  NRF_TIMER2->CC[0] = 30000;  // first toggle in ~30 ms
  NRF_TIMER2->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(TIMER2_IRQn, 7);  // low priority
  NVIC_ClearPendingIRQ(TIMER2_IRQn);
  NVIC_EnableIRQ(TIMER2_IRQn);
  NRF_TIMER2->TASKS_CLEAR = 1;
  NRF_TIMER2->TASKS_START = 1;
}

static void stopBootFlicker() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk;
  NVIC_DisableIRQ(TIMER2_IRQn);
  NVIC_ClearPendingIRQ(TIMER2_IRQn);
  digitalWrite(PIN_LED_BLUE, LOW);
  s_flicker_blue_on = false;
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

  // Boot indicator: both LEDs full bright for 1 s.
  digitalWrite(PIN_LED_RED,  HIGH);
  digitalWrite(PIN_LED_BLUE, HIGH);
  delay(1000);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);

  // 1-second gap, then red solid + blue disk-activity flicker. The flicker
  // runs in the background via TIMER2 until bootComplete() stops it.
  delay(1000);
  digitalWrite(PIN_LED_RED, HIGH);
  startBootFlicker();

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  delay(10); // give sx1262 some time to power up
}

void ThinkNodeM6Board::powerOff() {
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
  // Stop the disk-activity flicker, dark 1 s gap, then 100 ms blue flash.
  stopBootFlicker();
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
  delay(1000);
  digitalWrite(PIN_LED_BLUE, HIGH);
  delay(100);
  digitalWrite(PIN_LED_BLUE, LOW);
}

#endif
