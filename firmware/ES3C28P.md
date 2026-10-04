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
- the touch panel is polled about 3 times per frame (tied to the ~17-19 fps draw loop);
  swipes work, a separate fast sampling task would make strokes smoother
- sound plays: the amp enable is active low (`AMP_ACTIVE_LEVEL` 0)
- the boot log line `I2C (SDA 16, SCL 15) answers at:` lists 0x18 (ES8311) and 0x38
  (FT6336G): one bus for both
- about 17-19 fps (was 12-14, 2026-10-03). It was never the SPI link: scaling the 448x368 tank
  to 320x240 took ~37 ms of CPU a frame. Now (display_port_ili9341.c) the scaling is done as a
  row average two pixels a word, then a column average (about 4x less arithmetic, the two
  rounding directions cancel), on a flush task BELOW the tank task so the tank queues its next
  scene prefetch first and the scaling fills the time it spends waiting for it. What is left is
  memory contention with the language model: with the model idle a frame is ~19 fps, and the
  wait for the 330 KB scene-prefetch copy (~25 ms when the model streams weights, ~3 ms when
  it does not) is the largest piece; shrinking that means a renderer that restores less of the
  scene. The model's decisions are ~3% slower (3.6 s against 3.5 s)

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

- a very low cell connected alongside USB can pull the 5 V line down while it charges, and
  Windows then reports "USB device not recognized" (seen 2026-10-02): charge a deeply
  discharged cell on a proper charger first

## Presence sensor: the screen follows a person (POCKET_TANK_PRESENCE_VL53L1X)

A VL53L1X time-of-flight sensor lights the screen when somebody is near and darkens it
after they leave. Only the display and the sound go off: the tank keeps running (and the
model decides faster with nothing to draw, 14.4 tok/s against 12.6).

Wiring, to the 4-pin I2C header: 3V3, GND, SCL = GPIO15, SDA = GPIO16. The sensor answers
at 0x29 (no clash with 0x18 and 0x38). Optional: XSHUT to the GPIO14 pad (a hardware reset
the firmware pulses before it starts the sensor); the interrupt pad GPIO21 is reserved for
a later wake-on-approach and unused. Mount it facing outward at the bezel or edge.

Behaviour (`presence_port.h`):
- the screen is wanted while anyone is within the near distance (1000 mm in
  `sdkconfig.defaults.es3c28p`; the Kconfig default is 600), or for the hold time (30 s there;
  Kconfig default 20) after the last sign of anyone: a reading, a touch, or BOOT
- a touch on the dark screen wakes it at once (about 0.25 s to the picture); that touch
  is also a tap in the tank
- no sensor, or one that stops answering (3 bad readings): the screen stays ON, and a
  missing sensor is looked for every 5 s
- ranging cadence: every 0.5 s on USB and while the screen is on; every 5 s on battery with
  the screen off. A reading is ~18 mA for ~21 ms (measured; ~0.4 mC), so 5 s averages
  ~0.08 mA and 60 s ~0.007 mA against the tank's own 70-100 mA: the interval sets how
  soon a visitor lights the screen, not the battery life
- the sound is muted while the screen is off

Settings: `idf.py menuconfig` -> pocket-tank (near mm, hold s, battery period s, USB period
ms). Live, from the director console (not saved): `presence` (status), `presence fake <mm>`
or `fake off` (a made-up distance, so the screen logic can be tried with no sensor),
`near <mm>`, `hold <s>`, `bat <s>`, `usb <ms>`. Needs ESP-IDF 5.5.2+ for the driver
component (`grrtzm/vl53l1x_library`, ST's ultra-low-power API).

Checked on the board (2026-10-02). With fake distances: off after the hold time, on at
once for a near reading, a touch wakes it, off again after the hold, fail-open with no
sensor. With the real sensor on the I2C cable (no XSHUT wired): found at 0x29 beside the
codec (0x18) and touch (0x38) on the one bus, a single-shot reading takes ~21 ms (16-67),
a person at ~52 cm reads 517-527 mm (steady to ~5 mm), a hand at 20-26 cm reads 208-259,
an empty room reads "nothing valid" (-1) rather than a false near, a person at ~1 m reads
~1000 mm, and the screen followed all of it: dark 3 s (the test hold) after the last
reading under 600 mm, lit again as soon as someone returned.
At 1000 mm (2026-10-03, walking toward and away from the board with a 3 s test hold): readings
read valid out to at least 1.2 m; the screen stayed dark while the person hovered at 1.0-1.2 m
(1033-1211 mm), lit when they came inside 1 m (819 mm), stayed on at ~54 cm, and went dark
3 s after they backed off to ~1.16 m. A person standing right at the threshold does not make
the screen flicker, because the hold time bridges the gaps between readings.
NOT checked: the battery cadence (a 5 s reading interval with the screen off, which needs
the board running off the cell) and bright-sun or dark-clothing ranges (the long-range
config measures to ~1.3 m in ideal conditions; a dark target in sunlight reads shorter).

## Low-voltage cutoff (POCKET_TANK_CUTOFF_MV)

This board has no power-management chip, so nothing stops a cell being drained flat. On the
cell (not on a cable) and under the cutoff voltage for 30 s in a row (a load dip is not an
empty cell), the tank saves, darkens the panel and goes into deep sleep; BOOT or RESET wakes
it. The cutoff is 3300 mV in `sdkconfig.defaults.es3c28p` (`menuconfig` -> pocket-tank; 0 =
off), about empty for a good Li-ion cell at rest. It applies only to a board with no PMIC;
the AMOLED's AXP2101 board is left alone. The existing low-battery notice (10%) still comes
first. A deeply discharged cell plugged into USB can also stop Windows recognising the
board (see above), which is the other reason to keep the cell well above empty.

Director: `cutoff` (status), `cutoff <mV>` (set live, capped at 4200, not saved), `cutoff test`
(act as if the cell were under the cutoff, cable or not: 30 s, then save + deep sleep).

Checked on the board (2026-10-02, with `cutoff test` on USB, no cell connected): the count
starts at once, acts 30 s later, saves, and the board sleeps; the batlog holds the `off` row
(93%, 4110 mV), BOOT wakes it, and the tank comes back with its fish. The real trigger was
checked too (2026-10-02, a small test cell on the board, `cutoff 4200`, USB unplugged with a
person in front of the presence sensor): the screen went dark by itself after the 30 s, the
batlog `off` row reads the cell at 4099 mV, BOOT woke it with the fish intact, and the
restart put the cutoff back to 3300 mV. The test cutoff is not saved, so a restart always
returns to the compiled value. Not checked: a real discharge down to 3.3 V (hours), which
only the cutoff's threshold, not its trigger, depends on. After the cutoff the board is
asleep and does not wake by itself when USB is plugged in: press BOOT.
