# coproc_sleep

The JC3636K518 (a clone of the Waveshare ESP32-S3-Knob-Touch-LCD-1.8) carries
a second MCU, an **ESP32-U4WDH**, that drives the PCM5100A audio DAC. Knobby
never uses it, so it keeps running its factory firmware on battery. This sketch
replaces that firmware with one that deep-sleeps forever.

A CH445P switch routes the USB-C port to one chip or the other by **plug
orientation**: one way enumerates the S3 (`/dev/cu.usbmodem*`), flipped it
enumerates the ESP32 through a CH340 (`/dev/cu.usbserial-*`).

## Installing

Use the [web installer](https://knobby-mtg.github.io/knobby-mtg-life-counter/):
flip the plug and click "Put the audio chip to sleep". It works from Chrome or
Edge on any OS and refuses to flash the S3 side.

It takes no backup, so the factory firmware is gone for good. To keep a copy,
read the flash out with esptool before installing.

To flash from a checkout instead:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 tools/coproc_sleep
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 -p /dev/cu.usbserial-XXXX tools/coproc_sleep
```

Upload at 115200: this CH340 link drops out at the core's default 921600. A
sleeping chip can still be re-flashed, because the CH340 resets it into the
bootloader.

## DAC pins

The sketch also puts weak pull-downs on the DAC's I2S pins so the DAC can power
itself down. Those pin numbers come from the Waveshare schematic and are
unverified on K518 clones. Build with `-DPARK_DAC_PINS=0` to leave them
floating.

The JC3636K718 has not been checked for this chip.
