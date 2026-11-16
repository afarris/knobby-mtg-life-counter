/* Parks the secondary ESP32-U4WDH on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 * (and its JC3636K518 clone) in deep sleep forever. Knobby never uses this
 * Bluetooth-audio co-processor, so it otherwise drains the battery.
 * Flash it onto the ESP32 side of the USB-C switch (flip the plug; the port
 * shows up as /dev/cu.usbserial-*), NOT the ESP32-S3.
 * The web installer does this from a browser; see README.md. */

#include "esp_sleep.h"
#include "driver/rtc_io.h"

/* The PCM5100A DAC powers itself down when its I2S clocks stay low for more
 * than a second. In deep sleep the ESP32's pads float, so the clock inputs
 * could sit at any level and keep the DAC awake. Weak internal pull-downs,
 * latched with the RTC hold, keep them low without driving anything hard.
 *
 * The pin numbers are from the Waveshare schematic (BCK, DIN, LRCK, XSMT);
 * they are unverified on K518 clones. A pull-down is about 45 kOhm, so a
 * wrong pin costs at most ~70 uA and cannot fight another output.
 * Build with -DPARK_DAC_PINS=0 to leave the pins floating. */
#ifndef PARK_DAC_PINS
#define PARK_DAC_PINS 1
#endif

#if PARK_DAC_PINS
static const gpio_num_t dac_pins[] = {GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27, GPIO_NUM_32};
#endif

void setup()
{
#if PARK_DAC_PINS
  for (size_t i = 0; i < sizeof(dac_pins) / sizeof(dac_pins[0]); i++) {
    rtc_gpio_init(dac_pins[i]);
    rtc_gpio_set_direction(dac_pins[i], RTC_GPIO_MODE_DISABLED);
    rtc_gpio_pullup_dis(dac_pins[i]);
    rtc_gpio_pulldown_en(dac_pins[i]);
    rtc_gpio_hold_en(dac_pins[i]);
  }
#endif

  /* No wake source: only a reset or power cycle brings the chip back,
     and then it lands here again. */
  esp_deep_sleep_start();
}

void loop()
{
}
