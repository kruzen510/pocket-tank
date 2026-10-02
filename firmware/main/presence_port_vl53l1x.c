/* presence_port_vl53l1x.c — the screen follows a VL53L1X (ST's ultra-low-power API, the
 * grrtzm/vl53l1x_library component) on the ES3C28P's I2C header: 3V3, GND, SCL = GPIO15,
 * SDA = GPIO16, optional XSHUT = GPIO14. See presence_port.h for the behaviour.
 *
 * One task does everything slow: reset + init the sensor, then one single-shot reading
 * per cadence tick (start, wait for data-ready, read, clear). A reading is "near" when its
 * status is 0 and its distance is under the near threshold; anything else (out of range,
 * weak signal) is "nobody". The main loop only asks presence_port_screen_wanted(), which
 * is a comparison against a deadline - the loop never waits on the sensor. */
#include "presence_port.h"
#include "board_pins.h"
#include "sdkconfig.h"
#include "vl53l1x.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "presence";
#define SENSOR_ADDR      0x29
#define SENSOR_ID        0xEACC
#define FAILS_TO_GIVE_UP 3                    /* consecutive bad readings before the sensor counts as gone */
#define RETRY_MS         5000                 /* ... and how often a missing one is looked for again */

static i2c_master_bus_handle_t s_bus;
static vl53l1x_t s_sensor;
static bool s_have;                            /* s_sensor holds a device handle */
static volatile bool s_ok;                     /* the sensor is answering (task-owned) */
static volatile uint32_t s_hold_until_ms;      /* the screen is wanted until this (ms clock, compared signed) */
static volatile int s_mm = -1;                 /* last reading, -1 = none */
static volatile int s_fake = -1;               /* bench: a made-up distance (director `presence fake`), -1 = off */
static volatile int s_near_mm = CONFIG_POCKET_TANK_PRESENCE_NEAR_MM;
static volatile int s_hold_s  = CONFIG_POCKET_TANK_PRESENCE_HOLD_S;
static volatile int s_bat_s   = CONFIG_POCKET_TANK_PRESENCE_BATTERY_PERIOD_S;
static volatile int s_usb_ms  = CONFIG_POCKET_TANK_PRESENCE_USB_PERIOD_MS;
static volatile bool s_battery;                /* from main: on the cell, not the cable */
static volatile bool s_screen_on = true;       /* from screen_wanted(): picks the cadence */
static volatile int s_shot_ms;                 /* how long the last reading took (bench) */

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* extend the deadline, never shorten it (a reading must not cut short a longer hold) */
static void hold_for(uint32_t ms) {
    uint32_t t = ms_now() + ms;
    if ((int32_t)(t - s_hold_until_ms) > 0) s_hold_until_ms = t;
}

void presence_port_activity(void) { hold_for((uint32_t)s_hold_s * 1000u); }
bool presence_port_screen_wanted(void) {
    if (!s_ok && s_fake < 0) { s_screen_on = true; return true; }              /* no sensor: fail open */
    bool w = (int32_t)(ms_now() - s_hold_until_ms) < 0;
    s_screen_on = w;
    return w;
}
void presence_port_set_battery(bool on_battery) { s_battery = on_battery; }
int  presence_port_distance_mm(void) { return s_mm; }

/* reset (if XSHUT is wired), look for the sensor, bring it to the one configuration used here */
static bool sensor_start(void) {
    gpio_set_level(PIN_PRESENCE_XSHUT, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_PRESENCE_XSHUT, 1); vTaskDelay(pdMS_TO_TICKS(10));
    if (s_have) { vl53l1x_deinit(&s_sensor); s_have = false; }
    if (i2c_master_probe(s_bus, SENSOR_ADDR, 100) != ESP_OK) return false;
    if (vl53l1x_init(&s_sensor, s_bus, SENSOR_ADDR) != ESP_OK) return false;
    s_have = true;
    uint16_t id = 0;
    if (vl53l1x_get_sensor_id(&s_sensor, &id) != ESP_OK || id != SENSOR_ID) {
        ESP_LOGW(TAG, "something answers at 0x%02x but its id is 0x%04x, not a VL53L1X (0x%04x)", SENSOR_ADDR, id, SENSOR_ID);
        return false;
    }
    vl53l1x_stop(&s_sensor);                                                    /* idle, in case it was ranging when we reset */
    if (vl53l1x_sensor_init(&s_sensor) != ESP_OK) { ESP_LOGW(TAG, "VL53L1X init failed"); return false; }
    if (vl53l1x_config_long_100ms(&s_sensor) != ESP_OK) { ESP_LOGW(TAG, "VL53L1X config failed"); return false; }
    return true;
}

/* one single-shot reading: true = the exchange worked (mm = -1 for "nothing valid in range") */
static bool read_once(int *mm) {
    int64_t t0 = esp_timer_get_time();
    if (vl53l1x_start_single_shot(&s_sensor) != ESP_OK) return false;
    vl53l1x_result_t r = {0};
    if (vl53l1x_read(&s_sensor, &r, 800) != ESP_OK) return false;
    s_shot_ms = (int)((esp_timer_get_time() - t0) / 1000);
    *mm = (r.status == 0) ? (int)r.distance_mm : -1;
    return true;
}

static void presence_task(void *arg) {
    (void)arg;
    int fails = 0;
    for (;;) {
        int mm = -1; bool have_reading = false;
        if (s_fake >= 0) { mm = s_fake; have_reading = true; }                  /* bench: no sensor needed */
        else if (s_ok) {
            if (read_once(&mm)) { have_reading = true; fails = 0; }
            else if (++fails >= FAILS_TO_GIVE_UP) { s_ok = false; s_mm = -1; ESP_LOGW(TAG, "the sensor stopped answering: screen held on, looking for it every %d s", RETRY_MS / 1000); }
        } else if (sensor_start()) {
            s_ok = true; fails = 0;
            ESP_LOGI(TAG, "VL53L1X up at 0x%02x: screen near < %d mm, off %d s after the last sign; ranging every %d ms on USB / %d s on battery with the screen off",
                     SENSOR_ADDR, s_near_mm, s_hold_s, s_usb_ms, s_bat_s);
            continue;
        } else { vTaskDelay(pdMS_TO_TICKS(RETRY_MS)); continue; }

        if (have_reading) {
            s_mm = mm;
            if (mm >= 0 && mm < s_near_mm) hold_for((uint32_t)s_hold_s * 1000u);
        }
        bool slow = s_battery && !s_screen_on && s_fake < 0;
        vTaskDelay(pdMS_TO_TICKS(slow ? (uint32_t)s_bat_s * 1000u : (uint32_t)s_usb_ms));
    }
}

bool presence_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    s_bus = bus;
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_PRESENCE_XSHUT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_PRESENCE_XSHUT, 1);
    hold_for((uint32_t)s_hold_s * 1000u);                                       /* the screen is on at boot, for the hold time at least */
    if (xTaskCreatePinnedToCore(presence_task, "presence", 4096, NULL, 2, NULL, 0) != pdPASS) { ESP_LOGE(TAG, "no task"); return false; }
    ESP_LOGI(TAG, "presence sensor task started (VL53L1X at 0x%02x, XSHUT GPIO%d); the screen stays on until it is found", SENSOR_ADDR, PIN_PRESENCE_XSHUT);
    return true;
}

/* director: presence | presence fake <mm>|off | presence near <mm> | presence hold <s> | presence bat <s> | presence usb <ms> */
void presence_port_command(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[0], "fake")) {
        s_fake = !strcmp(argv[1], "off") ? -1 : atoi(argv[1]);
        if (s_fake >= 0) { s_mm = s_fake; if (s_fake < s_near_mm) presence_port_activity(); }
    } else if (argc >= 2 && !strcmp(argv[0], "near")) { int v = atoi(argv[1]); if (v >= 50 && v <= 4000) s_near_mm = v; }
    else if (argc >= 2 && !strcmp(argv[0], "hold"))   { int v = atoi(argv[1]); if (v >= 1 && v <= 3600) { s_hold_s = v; s_hold_until_ms = ms_now() + (uint32_t)v * 1000u; } }   /* bench: also restarts the countdown from now */
    else if (argc >= 2 && !strcmp(argv[0], "bat"))    { int v = atoi(argv[1]); if (v >= 1 && v <= 3600) s_bat_s = v; }
    else if (argc >= 2 && !strcmp(argv[0], "usb"))    { int v = atoi(argv[1]); if (v >= 50 && v <= 10000) s_usb_ms = v; }
    uint32_t left = (uint32_t)((int32_t)(s_hold_until_ms - ms_now()));
    ESP_LOGI(TAG, "sensor %s%s | last %d mm (a reading took %d ms) | near < %d mm, hold %d s (%s, %d s left) | cadence %d ms USB, %d s on battery with the screen off | now on %s, screen %s",
             s_ok ? "answering" : "NOT answering (screen held on)", s_fake >= 0 ? ", FAKE distance active" : "",
             s_mm, s_shot_ms, s_near_mm, s_hold_s, (int32_t)left > 0 ? "someone near" : "nobody", (int)((int32_t)left > 0 ? left / 1000 : 0),
             s_usb_ms, s_bat_s, s_battery ? "battery" : "the cable", s_screen_on ? "wanted" : "not wanted");
}
