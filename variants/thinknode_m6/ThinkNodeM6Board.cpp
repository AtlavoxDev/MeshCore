#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>
#include <helpers/LEDSequence.h>
#include <helpers/PowerButton.h>

// Power-button hold threshold. PowerButton::poll() drives the canonical
// mid-hold flash feedback (proportional to this threshold) on PIN_LED_RED
// and returns true once the button has been held for the full duration.
#define M6_OFF_COMMIT_MS  2000

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

  // Pin init — LED pins MUST be initialized before LEDSequence::playBoot()
  // so the helper can drive them. P_LORA_TX_LED happens to map to PIN_LED_BLUE
  // on this variant; we initialize it explicitly here BEFORE playBoot() so
  // it can't stomp on the BOTH_BRIGHT state.
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  pinMode(PIN_LED_RED,  OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif
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

  // Kick off the canonical boot LED sequence via the shared framework helper.
  // Non-blocking — TIMER2 drives the entire sequence (BRIGHT → DARK → FLICKER
  // → DARK → FLASH → off) in the background while setup() continues with
  // radio/SPI/BLE init. onBootComplete() will short-circuit the FLICKER phase
  // at the end of setup().
  LEDSequence::Config led_cfg;
  led_cfg.primary_pin   = PIN_LED_RED;
  led_cfg.secondary_pin = PIN_LED_BLUE;
  led_cfg.buzzer_pin    = -1;       // M6 has no buzzer
  led_cfg.active_level  = HIGH;     // M6 LEDs are active-high
  LEDSequence::begin(led_cfg);
  LEDSequence::playBoot();

  PowerButton::Config btn_cfg;
  btn_cfg.pin           = PIN_USER_BTN;
  btn_cfg.threshold_ms  = M6_OFF_COMMIT_MS;
  btn_cfg.feedback_pin  = PIN_LED_RED;  // mid-hold flashes use the red LED
  btn_cfg.active_level  = HIGH;
  PowerButton::begin(btn_cfg);

  Wire.begin();

  delay(10);  // give sx1262 some time to power up
}

void ThinkNodeM6Board::powerOff() {
  // Stop the boot sequence if it's still running, then play the canonical
  // shutdown cue (1 s solid + 50 ms both-flash + off).
  LEDSequence::cancel();

#ifdef P_LORA_TX_LED
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  LEDSequence::playPowerOff();

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

void ThinkNodeM6Board::onBootComplete() {
  // Signal the framework's boot sequence to exit FLICKER and play out the
  // remaining DARK + FLASH phases. Returns immediately — the visual
  // continues in the background via the TIMER2 ISR.
  LEDSequence::onBootComplete();
}

void ThinkNodeM6Board::pollButton() {
  // PowerButton handles button polling, hold-time tracking, and the
  // canonical mid-hold flash feedback. Configured in begin().
  if (PowerButton::poll()) {
    Serial.println("Powering off...");
    powerOff();  // does not return
  }
}

#endif
