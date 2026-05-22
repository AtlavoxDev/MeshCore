#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>

// --- Boot-phase "disk activity" blue LED flicker ---
// TIMER2 fires at pseudo-random intervals (10-100 ms); ISR toggles the blue
// LED. Runs autonomously throughout setup() so the user gets continuous
// "device is working" feedback during blocking calls like the_mesh.begin().
// TIMER0 is reserved by SoftDevice; TIMER1 is often used by other libraries;
// TIMER2 is reliably free in the M6 repeater build.
static volatile bool s_flicker_blue_on = false;
static uint32_t s_flicker_rng = 0xC0FFEE42;

static inline uint32_t flicker_next_rand() {
  // xorshift32 — cheap and good enough for "disk activity" jitter
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

    // Next toggle in 10-100 ms.
    NRF_TIMER2->CC[0] = 10000 + (flicker_next_rand() % 90000);
  }
}

static void startBootFlicker() {
  NRF_TIMER2->TASKS_STOP = 1;
  NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit << TIMER_BITMODE_BITMODE_Pos;
  NRF_TIMER2->PRESCALER = 4;  // 16 MHz / 2^4 = 1 MHz tick (1 µs)
  NRF_TIMER2->CC[0] = 30000;  // first toggle in ~30 ms
  NRF_TIMER2->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(TIMER2_IRQn, 7);  // low priority — don't block radio/serial
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
// SoftDevice may not be enabled in non-BLE builds (repeater), so we fall back
// to a direct register write — mirrors NRF52Board::enterSystemOff().
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
  NVIC_SystemReset();  // unreachable in normal flow
}

// Captured by an early constructor in variant.cpp before SystemInit()'s
// errata-136 workaround scrubs the RESETPIN bit. g_m6_was_powered is the
// GPREGRET2-based "did firmware run in this power session?" flag — false
// means power was completely lost (e.g., battery died, solar recharge),
// which we treat as a recovery boot.
extern volatile uint32_t g_m6_reset_reason;
extern volatile bool     g_m6_was_powered;

void ThinkNodeM6Board::begin() {
  NRF52Board::begin();

  // PIN_PWR_EN (peripheral power rail) is already driven HIGH by
  // initVariant() in variant.cpp; no need to re-assert it here.

  // NOTE: do not call analogWrite() here. On the Adafruit nRF52 core,
  // analogWrite() routes the pin to the PWM peripheral and subsequent
  // digitalWrite() calls no longer drive the pin. initVariant() already left
  // both LEDs LOW via digitalWrite, so we can use digitalWrite the whole way
  // through the boot sequence. powerOff() uses analogWrite later, after the
  // boot LEDs are no longer needed.
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  pinMode(PIN_LED_RED,  OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
  delay(20);  // pin settle / debounce

  // Decide whether to actually boot based on three signals:
  //   - RESETPIN bit set → user pressed reset → boot
  //   - OFF bit set → SENSE-LOW wake (only configured on PIN_USER_BTN) →
  //     user pressed the Function Button → boot
  //   - !g_m6_was_powered → GPREGRET2 magic was cleared by full power loss,
  //     so this is a recovery from a dead battery / solar recharge → boot
  //     automatically (important for unattended deployments)
  // Otherwise (POR/brownout/USB transient while we were already running) →
  // re-sleep silently.
  bool from_reset_pin   = (g_m6_reset_reason & POWER_RESETREAS_RESETPIN_Msk) != 0;
  bool from_button_wake = (g_m6_reset_reason & POWER_RESETREAS_OFF_Msk)      != 0;
  bool from_power_loss  = !g_m6_was_powered;

  if (!from_reset_pin && !from_button_wake && !from_power_loss) {
    // No deliberate user action and we were already powered — re-sleep.
    enterDeepSleep();
  }

  // Boot indicator: both LEDs full bright for 1 second.
  digitalWrite(PIN_LED_RED,  HIGH);
  digitalWrite(PIN_LED_BLUE, HIGH);
  delay(1000);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);

  // 1-second gap (LEDs off), then start the boot-phase indicator: red solid
  // on, blue flickering like a disk-drive activity LED. The flicker runs on
  // a TIMER2 interrupt so it keeps going through every blocking call in
  // setup() (the_mesh.begin() etc.). It's stopped by bootComplete() at the end.
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

  // Shutdown cue: red full bright for 1 s.
  analogWrite(PIN_LED_BLUE, 0);
  analogWrite(PIN_LED_RED,  255);
  delay(1000);
  analogWrite(PIN_LED_RED, 0);

  // Final "goodbye": brief both-LED flash so the user knows we're committed
  // and the device is going dark right now.
  analogWrite(PIN_LED_RED,  255);
  analogWrite(PIN_LED_BLUE, 255);
  delay(50);
  analogWrite(PIN_LED_RED,  0);
  analogWrite(PIN_LED_BLUE, 0);

  // If the button is still held, we can't yet enter SYSTEMOFF (SENSE-LOW
  // would wake us immediately). Wait silently for release — no LEDs lit,
  // so the wait is invisible.
  while (digitalRead(PIN_USER_BTN) == LOW) delay(10);

  Serial.flush();
  delay(50);
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
  // Stop the flicker timer, turn red off, pause 1 second (everything dark)
  // to clearly delimit the boot phase, then flash blue for 100 ms as the
  // "device is up" signal.
  stopBootFlicker();
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
  delay(1000);
  digitalWrite(PIN_LED_BLUE, HIGH);
  delay(100);
  digitalWrite(PIN_LED_BLUE, LOW);
}

#endif
