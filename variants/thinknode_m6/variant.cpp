#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"
#include "nrf.h"

// Globals captured early — before SystemInit()'s errata-136 workaround
// clears the RESETPIN bit, and before any C++ static constructors. Priority
// 101 runs before SystemInit (102).
//
// g_m6_reset_reason: snapshot of NRF_POWER->RESETREAS.
// g_m6_was_shutdown: true if GPREGRET2 holds the "user-intent" magic byte.
//
// GPREGRET2 is used as a user-intent flag: powerOff() writes 0xA5 right
// before SYSTEMOFF, board.begin() clears it on a successful boot. It
// persists across SYSTEMOFF but is wiped by a true power-on reset.
//
// NOTE: GPREGRET2 is also written by NRF52Board::enterSystemOff() under the
// NRF52_POWER_MANAGEMENT build flag. That flag is not set for any M6 env;
// if it ever is, the byte stored here will collide.
volatile uint32_t g_m6_reset_reason  = 0;
volatile bool     g_m6_was_shutdown  = false;

extern "C" __attribute__((constructor(101))) void m6_capture_resetreas(void) {
  g_m6_reset_reason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = 0xFFFFFFFFul;  // clear sticky bits
  g_m6_was_shutdown = (NRF_POWER->GPREGRET2 == 0xA5);
}

const uint32_t g_ADigitalPinMap[] = {
  0xff, 0xff, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
  27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
  40, 41, 42, 43, 44, 45, 46, 47
};

void initVariant() {
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);

  pinMode(QSPI_FLASH_EN, OUTPUT);
  digitalWrite(QSPI_FLASH_EN, HIGH);

  // For now stick adc_ctrl to fixed value
  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, LOW);

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  digitalWrite(PIN_LED_BLUE, LOW);
  digitalWrite(PIN_LED_RED, LOW);

  // gps
  pinMode(PIN_GPS_STANDBY, OUTPUT);
  digitalWrite(PIN_GPS_STANDBY, HIGH);
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, HIGH);
  pinMode(PIN_GPS_RESET, OUTPUT);
  digitalWrite(PIN_GPS_RESET, HIGH);
}
