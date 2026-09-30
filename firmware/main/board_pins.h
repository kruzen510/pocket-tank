/* board_pins.h — which board the firmware drives.
 *   default: Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1: SH8601 + FT3168; V2: CO5300 + CST816)
 *   CONFIG_POCKET_TANK_BOARD_LCD169: Waveshare ESP32-S3-(Touch-)LCD-1.69 (ST7789V2 240x280
 *     over SPI + CST816T), pins from Waveshare's HARDWARE_REFERENCE.md
 *     (schematic V2.1 - the "new version" with the model name printed on it).
 *   CONFIG_POCKET_TANK_BOARD_ES3C28P: Hosyond / LCDWIKI "2.8inch ESP32-S3 Display" (ES3C28P,
 *     ILI9341V 240x320 over SPI + FT6336G, ESP32-S3 N16R8), pins from the LCDWIKI hardware
 *     page (www.lcdwiki.com/2.8inch_ESP32-S3_Display) and checked against the board's silk. */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H
#include "sdkconfig.h"

#if defined(CONFIG_POCKET_TANK_BOARD_LCD169)
#define PIN_LCD_DC        4
#define PIN_LCD_CS        5
#define PIN_LCD_SCLK      6
#define PIN_LCD_MOSI      7
#define PIN_LCD_RST       8
#define PIN_LCD_BL        15
#define PIN_I2C_SDA       11
#define PIN_I2C_SCL       10
#define PIN_TP_RST        13
#define PIN_TP_INT        14
#define PIN_BAT_ADC       1        /* B+ through 200k/100k: VBAT = 3 x pin */
#define PIN_SYS_OUT       40       /* the PWR key, low while pressed */
#define PIN_SYS_EN        41       /* high = keep the battery switched on; low = power off (on battery) */
#define PIN_BUZZER        42       /* passive buzzer; keep LOW when silent */
#define I2C_ADDR_CST816   0x15
#define I2C_ADDR_FT3168   0x38     /* not on this board; touch_port still names it */
#define LCD169_PANEL_W    240      /* native portrait */
#define LCD169_PANEL_H    280
#define LCD169_Y_GAP      20       /* the ST7789's 320-row RAM, 280 rows visible */
#define LCD169_VIEW_W     280      /* the tank on this panel: landscape, scaled */
#define LCD169_VIEW_H     240
/* touch_port: the CST816T reports in the panel's portrait frame */
#define PANEL_W           LCD169_PANEL_W
#define PANEL_H           LCD169_PANEL_H

#elif defined(CONFIG_POCKET_TANK_BOARD_ES3C28P)
#define PIN_LCD_CS        10       /* the LCD sits on FSPI's IO_MUX pins (10 CS, 11 MOSI, 12 SCLK, 13 MISO) */
#define PIN_LCD_MOSI      11
#define PIN_LCD_SCLK      12
#define PIN_LCD_MISO      13       /* wired, unused: the panel is write-only here */
#define PIN_LCD_DC        46       /* a strapping pin (ROM log): fine as an output once running */
#define PIN_LCD_RST       -1       /* tied to the chip's EN: the panel resets with the ESP32-S3 */
#define PIN_LCD_BL        45       /* high = backlight on; a strapping pin (VDD_SPI), PWM from the app */
#define PIN_I2C_SDA       16       /* one bus: FT6336G touch, ES8311 codec, the 1.25 mm I2C header */
#define PIN_I2C_SCL       15
#define PIN_TP_RST        18
#define PIN_TP_INT        17
#define I2C_ADDR_FT3168   0x38     /* the FT6336G answers as the FT5x06 family */
#define I2C_ADDR_CST816   0x15     /* not on this board; touch_port still names it */
#define PIN_BAT_ADC       9        /* ADC1 ch8: the cell through a divider */
#define BAT_DIV_NUM       2        /* VBAT = pin x NUM / DEN: a 1:1 divider; a cell at 4.1 V (meter) read 4.12 V here, 2026-09-30 */
#define BAT_DIV_DEN       1
#define PIN_RGB_LED       42       /* one WS2812B: never driven, so it stays dark */
/* ES8311 on its own I2S pins, an FM8002E speaker amp; the amp's enable is ACTIVE LOW per
 * the LCDWIKI page (IO1 low = amp on); confirmed by ear on the board, 2026-09-30 */
#define PIN_I2S_MCLK      4
#define PIN_I2S_BCLK      5
#define PIN_I2S_WS        7
#define PIN_I2S_DOUT      8        /* ESP -> codec DSDIN */
#define PIN_I2S_DIN       6        /* codec ASDOUT (the mic; unused) */
#define PIN_AMP_EN        1
#define AMP_ACTIVE_LEVEL  0
#define ES3C28P_PANEL_W   240      /* native portrait */
#define ES3C28P_PANEL_H   320
#define ES3C28P_VIEW_W    320      /* the tank on this panel: landscape, scaled (x 0.714, y 0.652) */
#define ES3C28P_VIEW_H    240
/* Panel orientation (ILI9341 MADCTL after swap_xy) and colour: bench-verified 2026-09-30
 * (upright with USB-C on the right; POCKET_TANK_ES3C28P_FLIP turns it 180 degrees). */
#define ES3C28P_MIRROR_X  0
#define ES3C28P_MIRROR_Y  0
#define ES3C28P_INVERT    1        /* IPS panel: inversion on */
#ifdef CONFIG_POCKET_TANK_ES3C28P_FLIP
#define ES3C28P_FLIP      1
#else
#define ES3C28P_FLIP      0
#endif
/* Touch: the FT6336G reports x 0..239, y 0..319 in the panel's native portrait frame. These two map a
 * raw point to the view (landscape 320x240, the UPRIGHT side up); an inverted screen mirrors both.
 * Bench 2026-09-30, corner taps TL/TR/BR/BL read raw (200,9) (224,303) (18,8) (26,300): raw y runs with
 * the view's x, raw x runs AGAINST the view's y (the same single-axis flip the 1.69in board showed). */
#define ES3C28P_TOUCH_VX(rx, ry)  (ry)
#define ES3C28P_TOUCH_VY(rx, ry)  (ES3C28P_PANEL_W - 1 - (rx))
/* touch_port: raw range */
#define PANEL_W           ES3C28P_PANEL_W
#define PANEL_H           ES3C28P_PANEL_H

#else
#define PIN_LCD_CS        12
#define PIN_LCD_PCLK      11
#define PIN_LCD_DATA0     4
#define PIN_LCD_DATA1     5
#define PIN_LCD_DATA2     6
#define PIN_LCD_DATA3     7
#define PIN_I2C_SDA       15
#define PIN_I2C_SCL       14
#define PIN_TP_INT        21
#define I2C_ADDR_EXPANDER 0x20     /* TCA9554: bit0 LCD_RST, bit1 DSI_PWR_EN, bit2 TOUCH_RST, bit7 SD_CS */
#define I2C_ADDR_FT3168   0x38     /* V1 touch */
#define I2C_ADDR_CST816   0x15     /* V2 touch (probe => V2 board) */
#define PANEL_W           368      /* native portrait */
#define PANEL_H           448
#define V2_PANEL_X_GAP    0x10
/* ES8311 + NS4150B (resources/ESP32-S3-Touch-AMOLED-1.8.pdf) */
#define PIN_I2S_MCLK      16
#define PIN_I2S_BCLK      9
#define PIN_I2S_WS        45
#define PIN_I2S_DOUT      8        /* ESP -> codec DSDIN */
#define PIN_AMP_EN        46       /* NS4150B CTRL, 10k pulldown on the board */
#define AMP_ACTIVE_LEVEL  1
#endif
#endif
