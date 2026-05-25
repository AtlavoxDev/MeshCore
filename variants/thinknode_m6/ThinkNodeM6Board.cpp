#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>
#include <helpers/LEDSequence.h>
#include <helpers/HoldButton.h>

#define M6_OFF_COMMIT_MS  2000  // hold-to-power-off threshold

// Arm function button as SENSE-LOW wake source and enter SYSTEMOFF.
// Falls back to direct register write on non-SoftDevice builds.
static void enterDeepSleep() {
  nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_USER_BTN]),
                           NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    if (sd_power_system_off() == NRF_ERROR_SOFTDEVICE_NOT_ENABLED) sd_enabled = 0;
  }
  if (!sd_enabled) NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
  NVIC_SystemReset();  // unreachable
}

// Captured by variant.cpp's early constructor (before errata-136 clears RESETPIN).
extern volatile uint32_t g_m6_reset_reason;
extern volatile bool     g_m6_was_shutdown;

void ThinkNodeM6Board::begin() {
  NRF52Board::begin();

  pinMode(PIN_USER_BTN, INPUT_PULLUP);
  pinMode(PIN_LED_RED,  OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
#ifdef P_LORA_TX_LED
  // Init BEFORE playBoot() — P_LORA_TX_LED == PIN_LED_BLUE on M6, so writing
  // LOW here after playBoot() would stomp on the BOTH_BRIGHT state.
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif
  delay(20);  // pin settle

  // Boot decision: stay asleep if user deliberately powered off and isn't
  // asking to come back (no button-wake, no reset-pin press).
  bool from_reset_pin   = (g_m6_reset_reason & POWER_RESETREAS_RESETPIN_Msk) != 0;
  bool from_button_wake = (g_m6_reset_reason & POWER_RESETREAS_OFF_Msk)      != 0;

  if (g_m6_was_shutdown && !from_reset_pin && !from_button_wake) {
    enterDeepSleep();
  }

  NRF_POWER->GPREGRET2 = 0;  // clear user-intent flag; next reset is clean

  LEDSequence::Config led_cfg;
  led_cfg.primary_pin   = PIN_LED_RED;
  led_cfg.secondary_pin = PIN_LED_BLUE;
  led_cfg.buzzer_pin    = -1;        // no buzzer on M6
  led_cfg.active_level  = HIGH;
  LEDSequence::begin(led_cfg);
  LEDSequence::playBoot();

  HoldButton::Config btn_cfg;
  btn_cfg.pin           = PIN_USER_BTN;
  btn_cfg.threshold_ms  = M6_OFF_COMMIT_MS;
  btn_cfg.feedback_pin  = PIN_LED_RED;
  btn_cfg.active_level  = HIGH;
  HoldButton::begin(btn_cfg);

  Wire.begin();
  delay(10);  // sx1262 power-up settle
}

void ThinkNodeM6Board::powerOff() {
  LEDSequence::cancel();  // stop boot sequence before taking over LED pins

#ifdef P_LORA_TX_LED
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  LEDSequence::playPowerOff();

  // Wait for button release before SYSTEMOFF, else SENSE-LOW wakes immediately.
  while (digitalRead(PIN_USER_BTN) == LOW) delay(10);

  Serial.flush();
  delay(50);

  NRF_POWER->GPREGRET2 = 0xA5;  // user-intent magic byte
  enterDeepSleep();
}

uint16_t ThinkNodeM6Board::getBattMilliVolts() {
  digitalWrite(PIN_ADC_CTRL, HIGH);
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(10);

  int adcvalue = analogRead(PIN_VBAT_READ);  // 0..4095 over 0..3000mV
  digitalWrite(PIN_ADC_CTRL, LOW);
  return (uint16_t)((float)adcvalue * REAL_VBAT_MV_PER_LSB);  // compensated for divider
}

void ThinkNodeM6Board::onBootComplete() {
  LEDSequence::onBootComplete();
}

void ThinkNodeM6Board::pollButton() {
  if (HoldButton::poll()) {
    Serial.println("Powering off...");
    powerOff();  // does not return
  }
}

#endif
