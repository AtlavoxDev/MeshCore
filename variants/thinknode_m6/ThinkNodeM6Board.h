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

private:
  unsigned long _btn_down_at = 0;  // function-button press timestamp (0 = not pressed)

public:
  ThinkNodeM6Board() : NRF52Board("THINKNODE_M6_OTA") {}
  void begin();
  uint16_t getBattMilliVolts() override;

  // Called at the end of setup(). Signals the boot LED state machine to
  // exit the flicker phase; the final dark gap + blue flash run async via
  // the TIMER2 ISR. Returns immediately.
  void onBootComplete() override;

  // Polls the function button. Drives LED feedback during a hold and
  // calls powerOff() internally on a long press (>= 2 s).
  void pollButton() override;

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

  void powerOff() override;
};
