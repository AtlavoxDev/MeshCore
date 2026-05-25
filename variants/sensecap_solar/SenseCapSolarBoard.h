#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/NRF52Board.h>

class SenseCapSolarBoard : public NRF52BoardDCDC {
protected:
#ifdef NRF52_POWER_MANAGEMENT
  void initiateShutdown(uint8_t reason) override;
#endif

private:
#ifdef PIN_PWR_BTN
  unsigned long _pwr_btn_down_at = 0;  // PWR-button press timestamp (0 = not pressed)
  static constexpr unsigned long PWR_BTN_HOLD_OFF_MILLIS = 1500;
#endif

public:
  SenseCapSolarBoard() : NRF52Board("SENSECAP_SOLAR_OTA") {}
  void begin();

#ifdef PIN_PWR_BTN
  // Hold the PWR button for >= 1.5s to power off.
  void pollButton() override {
    int btnState = digitalRead(PIN_PWR_BTN);
    if (btnState == LOW) {
      if (_pwr_btn_down_at == 0) {
        _pwr_btn_down_at = millis();
      } else if ((unsigned long)(millis() - _pwr_btn_down_at) >= PWR_BTN_HOLD_OFF_MILLIS) {
        Serial.println("Powering off...");
        powerOff();  // does not return
      }
    } else {
      _pwr_btn_down_at = 0;
    }
  }
#endif

#if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED off
  }
#endif

  uint16_t getBattMilliVolts() override {
    digitalWrite(VBAT_ENABLE, LOW);
    int adcvalue = 0;
    analogReadResolution(12);
    analogReference(AR_INTERNAL_3_0);
    delay(10);
    adcvalue = analogRead(BATTERY_PIN);
    return (adcvalue * ADC_MULTIPLIER * AREF_VOLTAGE) / 4.096;
  }

  const char* getManufacturerName() const override {
    return "Seeed SenseCap Solar";
  }

  void powerOff() override {
    digitalWrite(LED_WHITE, LOW);
    digitalWrite(LED_BLUE, LOW);

#ifdef PIN_PWR_BTN
    while (digitalRead(PIN_PWR_BTN) == LOW);
    // Keep pull-up enabled in system-off so the wake line doesn't float low.
    nrf_gpio_cfg_sense_input(digitalPinToInterrupt(g_ADigitalPinMap[PIN_PWR_BTN]), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
#endif

#ifdef NRF52_POWER_MANAGEMENT
    initiateShutdown(SHUTDOWN_REASON_USER);
#else
    sd_power_system_off();
#endif
  }
};
