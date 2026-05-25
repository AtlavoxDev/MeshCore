#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * LEDSequence — canonical boot / power-off LED choreography (plus optional buzzer).
 *
 * Provides a shared, opt-in helper for the visual + audio cues a device gives
 * the user at boot and at power-off. Replaces hand-rolled per-device LED state
 * machines with one source of truth.
 *
 * Supported platforms:
 *   - NRF52 (Adafruit nRF52 BSP): boot animation runs autonomously via TIMER2
 *     hardware interrupts, in parallel with setup()'s blocking radio/SPI/BLE init.
 *   - ESP32: boot animation runs autonomously via esp_timer callbacks (FreeRTOS-
 *     task-backed; no ISR restrictions).
 *   - Other (RP2040, STM32, etc.): degraded behavior — playBoot() turns LEDs on
 *     statically, onBootComplete() does a synchronous final flash and turns them
 *     off. No mid-boot animation, but still some indication. Can be promoted to
 *     full async support later by adding hardware-timer code per platform.
 *
 * Hardware variability handled automatically:
 *   - 0-LED boards (primary_pin == -1): all LED calls are no-ops.
 *   - Bluefruit BSP "disable blue" stub (a pin set to -1): that pin is skipped.
 *   - Active-low LEDs: pass active_level = LOW; helper handles polarity.
 *   - Boards without buzzers: leave buzzer_pin = -1; buzzer cues are skipped.
 *
 * Init order trap (do not violate):
 *   Call LEDSequence::begin() + playBoot() AFTER any other init code that
 *   writes to the same pins. On the M6, P_LORA_TX_LED is the same physical pin
 *   as the blue LED — initializing P_LORA_TX_LED after playBoot() would stomp
 *   on the BOTH_BRIGHT state.
 *
 * PWM-poisoning trap (do not violate):
 *   Never call analogWrite() on a pin you've handed to LEDSequence. The
 *   Adafruit nRF52 core latches the pin to PWM and subsequent digitalWrite()
 *   calls are ignored. Helper uses digitalWrite() exclusively. If you need to
 *   take over a shared pin (e.g., to use PWM for mid-hold feedback elsewhere),
 *   call LEDSequence::cancel() first to stop the sequence and free the pin.
 */
class LEDSequence {
public:
  struct Config {
    int8_t  primary_pin   = -1;    ///< primary LED (e.g., red / power indicator). -1 = no LED.
    int8_t  secondary_pin = -1;    ///< secondary LED (e.g., blue / TX indicator). -1 = single-LED board.
    int8_t  buzzer_pin    = -1;    ///< optional buzzer pin. -1 = no buzzer cues.
    uint8_t active_level  = HIGH;  ///< HIGH on most boards; LOW for ~12 nRF52 boards w/ inverted LEDs.
  };

  /// Configure pin mapping. Must be called once before any other API call.
  /// Safe to call with primary_pin = -1 on 0-LED boards — all subsequent
  /// calls then become no-ops.
  static void begin(const Config& cfg);

  /// Kick off the boot LED sequence. Returns immediately; animation runs in
  /// the background via hardware timer (NRF52/ESP32) or is reduced to a
  /// static "LEDs on" cue (other platforms).
  ///
  /// Boot sequence: BRIGHT (1s) → DARK (1s) → FLICKER (until onBootComplete
  /// or 15s safety) → DARK (1s) → FLASH (100ms) → off.
  static void playBoot();

  /// Signal the FLICKER phase to exit early. Typically called from the
  /// board's onBootComplete() override at the end of setup(). The remaining
  /// phases (DARK + FLASH + off) play autonomously in the background.
  static void onBootComplete();

  /// Hard-stop any running boot sequence. Use before code that needs
  /// exclusive control of the LED pins (e.g., powerOff()'s analogWrite
  /// shutdown cue, or pollButton() taking over for mid-hold feedback).
  static void cancel();

  /// Synchronous power-off cue. Blocks ~1.05 seconds, then returns.
  /// Sequence: primary solid (1s) → both LEDs flash (50ms) → off.
  /// Plays buzzer descending shutdown tone if buzzer_pin is configured.
  static void playPowerOff();

  /// Poll a power-button pin for hold-to-power-off with progressive LED
  /// feedback. Call once per loop iteration from the board's pollButton()
  /// override. Returns true once the button has been held continuously for
  /// at least threshold_ms — the board should then call its powerOff().
  ///
  /// Visual cadence (threshold split into 2 measures × 4 beats = 8 beats):
  ///   Measure 1, beat 1: single brief blink (the "I see you" cue).
  ///   Measure 1, beats 2-4: dark.
  ///   Measure 2, every beat: flash (4 escalating flashes leading to commit).
  ///   Measure 3, beat 1: caller invokes powerOff() (SOLID phase follows).
  ///
  /// At a 2000 ms threshold: each beat is 250 ms, each flash is 125 ms.
  /// At a 1500 ms threshold: each beat is ~187 ms, each flash is ~94 ms.
  ///
  /// Released before threshold: LEDs go off, internal hold tracker resets,
  /// returns false. Subsequent presses start over.
  ///
  /// Behaves correctly even if begin() was called with no LED pins —
  /// returns true at threshold regardless, so 0-LED boards still get
  /// hold-to-power-off functionality (just without visual feedback).
  static bool pollPowerButton(int8_t pin, uint32_t threshold_ms);
};
