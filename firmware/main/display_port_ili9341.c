/* display_port_ili9341.c — Hosyond / LCDWIKI ESP32-S3 2.8in (ES3C28P): ILI9341V,
 * 240x320 IPS over plain 4-wire SPI, PWM backlight on GPIO45.
 *
 * The tank still renders its native landscape 448x368 frame (common/render.c
 * is untouched); this port scales it to the panel's 320x240 landscape view
 * (x 0.714, y 0.652 - a ~9% vertical squash, no letterbox) with a 2x2 box
 * filter and streams it in DMA stripes. Unlike the ST7789 port next door, the
 * panel does the landscape rotation itself (MADCTL MV: esp_lcd swap_xy), so
 * the frame goes out row by row - no software rotation, and every read and
 * write here is sequential. The picture is flipped 180 degrees in software
 * (display_port_set_inverted, and POCKET_TANK_ES3C28P_FLIP for the board's
 * fixed "which edge is the bottom"); touch_port_ft3168.c undoes the same
 * mapping. The panel's own reset is the chip's EN, so there is no RST pin. */
#include "display_port.h"
#include "board_pins.h"
#include "tank.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "ili9341";
#define LCD_HOST     SPI2_HOST
#define LCD_PCLK_HZ  (40 * 1000 * 1000)       /* the ILI9341's practical ceiling; 320x240x16 = 31 ms a frame */
#define STRIPE_ROWS  20                        /* view rows per DMA transfer (240 / 20 = 12 stripes) */
#define VIEW_W       ES3C28P_VIEW_W
#define VIEW_H       ES3C28P_VIEW_H
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static i2c_master_bus_handle_t s_i2c;
static uint8_t s_brightness = 0xFF;
static bool s_bl_ready, s_asleep;
static bool s_lit;                              /* the first frame is on the glass: the backlight may come on */
static uint16_t *s_stripe[2];
static SemaphoreHandle_t s_stripe_free;
/* the flush pipeline: display_port_flush() only hands a frame to a task of its own, which scales it into the stripes and
 * queues the DMA, so the tank draws the next frame while this one is still on the wire (the 40 MHz SPI link needs ~31 ms
 * a frame; done in line it was the whole frame time). s_fb_free is given while no frame is being read. */
static TaskHandle_t s_flush_task;
static SemaphoreHandle_t s_fb_free;
static const uint16_t *volatile s_job_fb;
static volatile int s_job_f;
static void flush_task(void *arg);
static bool s_inverted;
/* view pixel -> the two tank columns / rows the box filter averages; [0] as drawn upright,
 * [1] for the 180-degree flipped picture (view index reversed) */
static uint16_t s_x0[2][VIEW_W], s_x1[2][VIEW_W];
static uint16_t s_y0[2][VIEW_H], s_y1[2][VIEW_H];

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }
bool board_is_v2(void) { return false; }        /* touch_port: this board's touch is an FT5x06-family FT6336G */

void display_port_set_inverted(bool inverted) { s_inverted = inverted; }

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx) {
    (void)io; (void)ev; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_stripe_free, &hp);
    return hp == pdTRUE;
}

static void bl_write(uint8_t level) {
    if (!s_bl_ready) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

static void bl_init(void) {
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                              .timer_num = BL_LEDC_TIMER, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_channel_config_t c = { .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_LEDC_CHANNEL,
                                .timer_sel = BL_LEDC_TIMER, .duty = 0, .hpoint = 0 };
    s_bl_ready = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
    if (!s_bl_ready) {                          /* no PWM: plain on/off */
        gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(PIN_LCD_BL, 0);
        ESP_LOGW(TAG, "backlight PWM unavailable: on/off only");
    }
}

static void panel_setup(void) {
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, ES3C28P_INVERT);
    esp_lcd_panel_swap_xy(s_panel, true);       /* landscape in hardware: view (x, y) = panel RAM (row, column) */
    esp_lcd_panel_mirror(s_panel, ES3C28P_MIRROR_X, ES3C28P_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);
}

/* FT6336G: pulse its reset (TP_RST is a plain GPIO here) and give it time to
 * boot before the first I2C poll. */
static void touch_reset(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(250));
}

/* bring-up aid: who is on the board's one I2C bus (expect the FT6336G at 0x38,
 * the ES8311 at 0x18) - it is how a wrong-bus codec shows up in the boot log */
static void i2c_scan_log(void) {
    char line[160]; int n = 0;
    line[0] = 0;
    for (int a = 0x08; a < 0x78 && n < 20; a++) {
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK) {
            size_t l = strlen(line);
            snprintf(line + l, sizeof line - l, " 0x%02x", a);
            n++;
        }
    }
    ESP_LOGI(TAG, "I2C (SDA %d, SCL %d) answers at:%s", PIN_I2C_SDA, PIN_I2C_SCL, n ? line : " nothing");
}

bool display_port_init(void) {
    i2c_master_bus_config_t bus = { .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
    touch_reset();
    i2c_scan_log();

    for (int f = 0; f < 2; f++) {
        for (int x = 0; x < VIEW_W; x++) {
            int c = f ? VIEW_W - 1 - x : x;                 /* the flipped picture shows the mirrored view column */
            int sx = c * TANK_W / VIEW_W;
            s_x0[f][x] = sx; s_x1[f][x] = sx + 1 < TANK_W ? sx + 1 : sx;
        }
        for (int y = 0; y < VIEW_H; y++) {
            int r = f ? VIEW_H - 1 - y : y;
            int sy = r * TANK_H / VIEW_H;
            s_y0[f][y] = sy; s_y1[f][y] = sy + 1 < TANK_H ? sy + 1 : sy;
        }
    }

    for (int i = 0; i < 2; i++) {
        s_stripe[i] = heap_caps_malloc(VIEW_W * STRIPE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_stripe[i]) { ESP_LOGE(TAG, "no DMA RAM for the stripes"); return false; }
    }
    s_stripe_free = xSemaphoreCreateCounting(2, 2);
    s_fb_free = xSemaphoreCreateBinary();
    xSemaphoreGive(s_fb_free);                  /* nothing in flight yet */

    bl_init();                                  /* dark until the first frame is on the glass */
    spi_bus_config_t spi = { .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = VIEW_W * STRIPE_ROWS * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg = { .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0,
        .trans_queue_depth = 4, .on_color_trans_done = on_trans_done };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));
    esp_lcd_panel_dev_config_t pcfg = { .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                                        .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &pcfg, &s_panel));
    panel_setup();
    ESP_LOGI(TAG, "panel up: %dx%d native portrait, tank %dx%d scaled to %dx%d landscape (swap_xy, mirror %d/%d, invert %d, flip %d)",
             ES3C28P_PANEL_W, ES3C28P_PANEL_H, TANK_W, TANK_H, VIEW_W, VIEW_H,
             ES3C28P_MIRROR_X, ES3C28P_MIRROR_Y, ES3C28P_INVERT, ES3C28P_FLIP);
    /* BELOW the tank task (4) on the tank's core: the tank queues its next scene prefetch the moment it has handed a frame
     * over, and the scaling then runs in the time the tank spends waiting (for that prefetch, and in its frame delay) instead
     * of hogging the core first - at the higher priority the tank sat idle ~26 ms a frame behind it (measured 2026-10-03) */
    if (xTaskCreatePinnedToCore(flush_task, "lcdflush", 4096, NULL, 3, &s_flush_task, 0) != pdPASS) { ESP_LOGE(TAG, "no flush task"); return false; }
    return true;
}

/* every frame read and on the wire: nothing in flight, the panel may be commanded. Only the tank task flushes,
 * so nothing new can start while this waits. */
static void flush_quiesce(void) {
    xSemaphoreTake(s_fb_free, portMAX_DELAY); xSemaphoreGive(s_fb_free);
    xSemaphoreTake(s_stripe_free, portMAX_DELAY); xSemaphoreTake(s_stripe_free, portMAX_DELAY);
    xSemaphoreGive(s_stripe_free); xSemaphoreGive(s_stripe_free);
}

void display_port_sleep(void) {
    if (!s_panel) return;
    flush_quiesce();
    bl_write(0);
    if (!s_bl_ready) gpio_set_level(PIN_LCD_BL, 0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
    s_asleep = true;
}

void display_port_wake(void) {
    if (!s_panel) return;
    flush_quiesce();
    esp_lcd_panel_disp_sleep(s_panel, false);
    panel_setup();
    s_asleep = false;
    display_port_set_brightness(s_brightness);
}

void display_port_set_brightness(uint8_t level) {
    s_brightness = level;
    if (s_asleep || !s_lit) return;
    if (s_bl_ready) bl_write(level);
    else gpio_set_level(PIN_LCD_BL, level ? 1 : 0);
}
uint8_t display_port_brightness(void) { return s_brightness; }

/* Per-channel average of RGB565 values without unpacking them: (a & b) + ((a ^ b) >> 1), the top bit of every
 * channel masked off the shifted half so nothing carries across channels (0x7BEF = R/B 01111, G 011111). That form
 * rounds each channel DOWN; (a | b) - ((a ^ b) >> 1) rounds UP. The row pass rounds down and the column pass up, so
 * the result is within one step of a true 2x2 mean and unbiased. The row pass works on two pixels per 32-bit word. */
#define AVG_MASK2 0x7BEF7BEFu
typedef uint32_t __attribute__((may_alias)) u32a;    /* the frame is uint16_t pixels, read two at a time */
typedef uint16_t __attribute__((may_alias)) u16a;
static uint16_t s_vrow[TANK_W] __attribute__((aligned(4)));   /* one source row pair, vertically averaged (internal RAM) */

/* view row vy, column vx = the 2x2 box average at tank (s_x0/1[vx], s_y0/1[vy]), through the table set for the
 * picture's side up - done in two cheap passes per view row: the two tank rows averaged across the whole width two
 * pixels a time, then neighbouring columns averaged at the table positions (about 4x less arithmetic than averaging
 * four pixels per output pixel). One stripe = STRIPE_ROWS view rows, written to the panel while the next one is scaled. */
static void flush_frame(const uint16_t *fb, int f) {
    const uint16_t *x0t = s_x0[f], *x1t = s_x1[f];
    int cur = 0;
    for (int vy0 = 0; vy0 < VIEW_H; vy0 += STRIPE_ROWS) {
        int rows = VIEW_H - vy0 < STRIPE_ROWS ? VIEW_H - vy0 : STRIPE_ROWS;
        xSemaphoreTake(s_stripe_free, portMAX_DELAY);
        uint16_t *stripe = s_stripe[cur];
        for (int r = 0; r < rows; r++) {
            const u32a *ra = (const u32a *)(fb + s_y0[f][vy0 + r] * TANK_W), *rb = (const u32a *)(fb + s_y1[f][vy0 + r] * TANK_W);
            u32a *va32 = (u32a *)s_vrow;
            for (int i = 0; i < TANK_W / 2; i++) {
                uint32_t u = ra[i], v = rb[i];
                va32[i] = (u & v) + (((u ^ v) >> 1) & AVG_MASK2);
            }
            const u16a *va = (const u16a *)s_vrow;
            uint16_t *dst = stripe + r * VIEW_W;
            for (int x = 0; x < VIEW_W; x++) {
                uint32_t p = va[x0t[x]], q = va[x1t[x]];
                uint32_t m = (p | q) - (((p ^ q) >> 1) & 0x7BEFu);         /* this pass rounds UP: the row pass rounded down, so the two cancel */
                dst[x] = (uint16_t)((m << 8) | (m >> 8));                  /* big-endian over SPI */
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, vy0, VIEW_W, vy0 + rows, stripe);
        if (err != ESP_OK) {
            xSemaphoreGive(s_stripe_free);
            static int logged;
            if (logged++ < 3) ESP_LOGE(TAG, "draw_bitmap vy0=%d: %s", vy0, esp_err_to_name(err));
        }
        cur ^= 1;
    }
    xSemaphoreGive(s_fb_free);                  /* every pixel of fb is read: the tank may draw into it again while the last stripes finish */
    if (!s_lit) {                               /* light the backlight only once there is a picture */
        s_lit = true;
        xSemaphoreTake(s_stripe_free, portMAX_DELAY); xSemaphoreTake(s_stripe_free, portMAX_DELAY);
        xSemaphoreGive(s_stripe_free); xSemaphoreGive(s_stripe_free);
        display_port_set_brightness(s_brightness);
    }
}

static void flush_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        flush_frame(s_job_fb, s_job_f);
    }
}

/* Hand the frame to the flush task and return: the buffer is the caller's again once the task has read it, which the
 * NEXT call waits for (the tank alternates two buffers, so by then it is the other one being drawn). */
void display_port_flush(const uint16_t *fb) {
    if (!s_panel || !s_flush_task) return;
    xSemaphoreTake(s_fb_free, portMAX_DELAY);   /* the previous frame is fully read */
    s_job_fb = fb;
    s_job_f = (s_inverted != (ES3C28P_FLIP != 0)) ? 1 : 0;
    xTaskNotifyGive(s_flush_task);
}
