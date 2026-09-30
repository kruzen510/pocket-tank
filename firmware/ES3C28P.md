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

## Bring-up: what was assumed and must be checked on the board

The pin table is the vendor's; these are not (all in `board_pins.h`):
- `ES3C28P_MIRROR_X/Y`, `ES3C28P_INVERT`, and the RGB order in `display_port_ili9341.c`:
  a mirrored, upside-down, or colour-swapped picture is one of these
- `ES3C28P_TOUCH_VX/VY`: the boot log prints `press: raw (x,y) -> tank (x,y)` for
  the first 40 presses; tap the four corners and the mapping falls out
- `BAT_DIV_NUM/DEN`: the divider on GPIO9 is assumed 1:1 (VBAT = 2 x pin);
  check against a meter on a real cell
- `AMP_ACTIVE_LEVEL`: the amp enable's polarity, checked by ear
- the boot log line `I2C (SDA 16, SCL 15) answers at:` should list 0x18 (ES8311)
  and 0x38 (FT6336G); if 0x18 is missing the codec is on another bus
