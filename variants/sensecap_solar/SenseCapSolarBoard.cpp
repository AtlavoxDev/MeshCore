#include <Arduino.h>
#include <Wire.h>

#include "SenseCapSolarBoard.h"
#include <helpers/LEDSequence.h>
#include <helpers/HoldButton.h>

#define SS_OFF_COMMIT_MS  2000  // hold-to-power-off threshold

#ifdef NRF52_POWER_MANAGEMENT
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

void SenseCapSolarBoard::initiateShutdown(uint8_t reason) {
  bool enable_lpcomp = (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
                        reason == SHUTDOWN_REASON_BOOT_PROTECT);

  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, enable_lpcomp ? LOW : HIGH);

  if (enable_lpcomp) {
    configureVoltageWake(power_config.lpcomp_ain_channel, power_config.lpcomp_refsel);
  }

  enterSystemOff(reason);
}
#endif // NRF52_POWER_MANAGEMENT

void SenseCapSolarBoard::begin() {
  NRF52BoardDCDC::begin();

  pinMode(BATTERY_PIN, INPUT);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0);
  delay(50);

#ifdef PIN_PWR_BTN
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);
#endif

#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif
  Wire.begin();

  pinMode(LED_WHITE, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  digitalWrite(LED_WHITE, LOW);
  digitalWrite(LED_BLUE,  LOW);

#ifdef P_LORA_TX_LED
  // Init BEFORE playBoot() — P_LORA_TX_LED maps to LED_BLUE on this variant,
  // so writing LOW here after playBoot() would stomp on the BOTH_BRIGHT state.
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

#ifdef NRF52_POWER_MANAGEMENT
  // Check battery voltage BEFORE the boot LED sequence — LEDs load the rail
  // and could sag below the bootlock threshold mid-boot, causing a partial
  // hang. On low voltage this enters SYSTEMOFF with no LEDs lit.
  checkBootVoltage(&power_config);
#endif

  LEDSequence::Config led_cfg;
  led_cfg.primary_pin   = LED_WHITE;   // solid during FLICKER + hold feedback
  led_cfg.secondary_pin = LED_BLUE;    // toggling in FLICKER + final FLASH
  led_cfg.buzzer_pin    = -1;          // no buzzer wired through helper
  led_cfg.active_level  = HIGH;
  LEDSequence::begin(led_cfg);
  LEDSequence::playBoot();

#ifdef PIN_PWR_BTN
  HoldButton::Config btn_cfg;
  btn_cfg.pin           = PIN_PWR_BTN;
  btn_cfg.threshold_ms  = SS_OFF_COMMIT_MS;
  btn_cfg.feedback_pin  = LED_WHITE;
  btn_cfg.active_level  = HIGH;
  HoldButton::begin(btn_cfg);
#endif

  delay(10);  // sx1262 power-up settle
}

void SenseCapSolarBoard::powerOff() {
  LEDSequence::cancel();  // stop boot sequence before taking over LED pins

#ifdef P_LORA_TX_LED
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  LEDSequence::playPowerOff();

#ifdef PIN_PWR_BTN
  // Wait for button release before SYSTEMOFF, else SENSE-LOW wakes immediately.
  while (digitalRead(PIN_PWR_BTN) == LOW) delay(10);
  // Keep pull-up enabled in system-off so the wake line doesn't float low.
  nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_PWR_BTN]),
                           NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
#endif

#ifdef NRF52_POWER_MANAGEMENT
  initiateShutdown(SHUTDOWN_REASON_USER);
#else
  sd_power_system_off();
#endif
}

void SenseCapSolarBoard::onBootComplete() {
  LEDSequence::onBootComplete();
}

void SenseCapSolarBoard::pollButton() {
#ifdef PIN_PWR_BTN
  if (HoldButton::poll()) {
    Serial.println("Powering off...");
    powerOff();  // does not return
  }
#endif
}
