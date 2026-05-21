#pragma once

#include <Arduino.h>
#include <MeshCore.h>
#include <helpers/NRF52Board.h>

// built-ins
#define VBAT_MV_PER_LSB   (0.73242188F)   // 3.0V ADC range and 12-bit ADC resolution = 3000mV/4096

#define VBAT_DIVIDER_COMP ADC_MULTIPLIER          // Compensation factor for the VBAT divider

#define PIN_VBAT_READ     BATTERY_PIN
#define REAL_VBAT_MV_PER_LSB (VBAT_DIVIDER_COMP * VBAT_MV_PER_LSB)

class ThinkNodeM6Board : public NRF52BoardDCDC {
protected:
#if NRF52_POWER_MANAGEMENT
  void initiateShutdown(uint8_t reason) override;
#endif

public:
  ThinkNodeM6Board() : NRF52Board("THINKNODE_M6_OTA") {}
  void begin();
  uint16_t getBattMilliVolts() override;
  void bootComplete();

#if defined(P_LORA_TX_LED)
  void onBeforeTransmit() override {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
  }
  void onAfterTransmit() override {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED off
  }
#endif

  const char* getManufacturerName() const override {
    return "Elecrow ThinkNode M6";
  }

  void powerOff() override {
    // Turn off LEDs so the device visually confirms a clean shutdown.
    digitalWrite(PIN_LED_RED,  LOW);
    digitalWrite(PIN_LED_BLUE, LOW);
    #ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, LOW);
    #endif

    // Break the soft-power latch — on battery, this physically cuts MCU power.
    digitalWrite(PIN_PWR_EN, LOW);

    // Belt-and-braces: if USB is providing power, the latch drop won't kill the chip.
    sd_power_system_off();

    while (1) {}  // unreachable
  }
};
