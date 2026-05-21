#include "ThinkNodeM6Board.h"
#include <Arduino.h>

#ifdef THINKNODE_M6

#include <Wire.h>

void ThinkNodeM6Board::begin() {
  NRF52Board::begin();

  // Soft-power latch: the Function Button momentarily applies VCC; firmware
  // must drive PIN_PWR_EN HIGH within the first few hundred ms to keep the
  // rail alive.
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);

  pinMode(PIN_LED_RED,  OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_RED,  HIGH);
  digitalWrite(PIN_LED_BLUE, HIGH);
  delay(1000);
  digitalWrite(PIN_LED_RED,  LOW);
  digitalWrite(PIN_LED_BLUE, LOW);

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  pinMode(PIN_USER_BTN, INPUT_PULLUP);

  delay(10); // give sx1262 some time to power up
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
  digitalWrite(PIN_LED_RED, HIGH); delay(150); digitalWrite(PIN_LED_RED, LOW);
  delay(120);
  digitalWrite(PIN_LED_BLUE, HIGH); delay(150); digitalWrite(PIN_LED_BLUE, LOW);
}
#endif
