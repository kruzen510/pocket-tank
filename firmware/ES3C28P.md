# Building for the Hosyond / LCDWIKI ESP32-S3 2.8" display (ES3C28P)

The 2.8" 240x320 capacitive-touch ESP32-S3 board sold by Hosyond (LCDWIKI model
ES3C28P; ES3N28P is the same board without touch). ESP32-S3-WROOM-1 N16R8: 16 MB
flash and 8 MB octal PSRAM, the memory the tank was sized for. Builds with
ESP-IDF v6.0.2 (tested) or v5.4.1. From an ESP-IDF shell:

    cd firmware
    idf.py fullclean            # or delete sdkconfig + build/ - old settings would win otherwise
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.es3c28p" set-target esp32s3 build
    idf.py -p COM21 flash monitor     # your port

The app flash leaves the model partition alone. If the board never had the
model written, write it once (the factory firmware's partition table is
replaced by ours, so do this on the first flash):

    python -m esptool --chip esp32s3 -p COM21 write_flash 0x290000 ../model/out/model_q4.bin

What differs from the AMOLED build (see `board_pins.h` for every pin):
- display: ILI9341V over SPI (40 MHz), the 448x368 tank box-filtered down to
  320x240 and rotated by the panel itself (MADCTL swap_xy); needs the
  `espressif/esp_lcd_ili9341` component, fetched by the first build
- touch: FT6336G at 0x38, read through the same FT5x06 driver; reset on GPIO18
- audio: the ES8311 + FM8002E amp are on this board's own I2S pins (MCLK 4,
  BCLK 5, WS 7, DOUT 8), the amp enable on GPIO1 is ACTIVE LOW
- power: no PMIC, no PWR key, no RTC, no IMU. VBAT by ADC (GPIO9); BOOT is the
  sleep key (the no-PMIC path); the sleep is deep sleep, BOOT wakes it
- the clock: with no RTC the chip's own clock carries the time through a deep
  sleep; a power loss restarts it from the build time and no absence is simulated
- which edge is the bottom is a build choice (no IMU): `POCKET_TANK_ES3C28P_FLIP`
  in menuconfig turns the picture and the touch 180 degrees together

## Bring-up status (bench, 2026-09-30, one board)

Checked on the board and by eye / ear:
- picture upright with the default settings, USB-C on the right; colours right
  (`ES3C28P_MIRROR_X/Y` 0/0, `ES3C28P_INVERT` 1, BGR element order); turn it with
  `POCKET_TANK_ES3C28P_FLIP`
- touch: calibrated with 12 crosshairs (a 4x3 grid in view px, a fingertip held on each),
  a straight line per axis, 2.9 px RMS error (the placement noise; a quadratic and cross
  terms did not help): `vx = 1.0105 * rawY - 3.36`, `vy = 248.66 - 1.1286 * rawX`
  (`ES3C28P_TOUCH_VX/VY`). The glass is bigger than the picture vertically: raw x 0..239
  spans view y 249 .. -21, so the top ~21 px and bottom ~9 px of the range are the bezel
  and clamp to the edge (a swipe can start on the bezel). The fit is to where a fingertip
  is aimed, so the AMOLED's 10 px "fingers land low" lift is off for this board
  (`touch bias` on the director still tunes it live). The first version of this map was
  a 1:1 flat-offset guess from four corner taps; it read 15-20 px too low at the top and
  up to 15 px too high at the bottom. The boot log prints `press: raw (x,y) -> tank (x,y)`
  for the first 40 presses, which is how to spot a unit that differs
- the touch panel is polled about 30-40 times a second (tied to the ~13 fps draw loop);
  swipes work, a separate fast sampling task would make strokes smoother
- sound plays: the amp enable is active low (`AMP_ACTIVE_LEVEL` 0)
- the boot log line `I2C (SDA 16, SCL 15) answers at:` lists 0x18 (ES8311) and 0x38
  (FT6336G): one bus for both
- about 13 fps: the 40 MHz SPI link needs ~32 ms a frame and runs after the render

- battery divider: `BAT_DIV_NUM/DEN` = 2/1 (VBAT = 2 x the GPIO9 pin, a 1:1 divider). A
  cell that measured 4.1 V on a meter read 4.12 V on the board (pin 2.06 V) with it
  connected and USB plugged in, so the ratio holds to the meter's resolution (0.1 V).
  Not measured: the board on battery alone, at a lower voltage, to check the slope

Worth knowing:
- with no cell plugged in the pin reads about 2.07 V (the charger's open output), almost
  the same as a charged cell, so the gauge shows a nearly full, charging battery: there is
  no way to tell "no cell" from a full one on this board
- "on the cable" is the chip's own USB seeing a host; a wall charger with no data lines
  looks like "on battery" (the level shown is still the voltage-based one)
